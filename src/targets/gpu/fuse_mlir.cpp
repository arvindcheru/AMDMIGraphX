/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2024 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <migraphx/gpu/fuse_mlir.hpp>
#include <migraphx/gpu/mlir.hpp>
#include <migraphx/matcher.hpp>
#include <migraphx/pass_manager.hpp>
#include <migraphx/make_op.hpp>
#include <migraphx/register_op.hpp>
#include <migraphx/env.hpp>
#include <migraphx/dead_code_elimination.hpp>
#include <migraphx/common.hpp>
#include <migraphx/algorithm.hpp>
#include <migraphx/param_utils.hpp>
#include <optional>

namespace migraphx {
inline namespace MIGRAPHX_INLINE_NS {

struct module;

namespace gpu {

MIGRAPHX_DECLARE_ENV_VAR(MIGRAPHX_ENABLE_EXTRA_MLIR);
MIGRAPHX_DECLARE_ENV_VAR(MIGRAPHX_ENABLE_MLIR_INPUT_FUSION);
MIGRAPHX_DECLARE_ENV_VAR(MIGRAPHX_ENABLE_MLIR_REDUCE_FUSION);
MIGRAPHX_DECLARE_ENV_VAR(MIGRAPHX_DISABLE_MLIR);
/**
 * @brief Declares a new MIGraphX environment variable which forces to generate
 * only specific MLIR operations.
 *
 * The variable, if defined, forces MIGraphX to use only specific operations
 * with MLIR regardless of the underlying GPU architecture. The variable accepts
 * a list of operations separated by comma. The variable recognizes the following
 * operations: "fused", "convolution", "dot". If the variable is not defined MIGraphX
 * will decide by itself which operations to delegate to MLIR. The variable is
 * intended to be primarily used by rocMLIR developers.
 */
MIGRAPHX_DECLARE_ENV_VAR(MIGRAPHX_MLIR_USE_SPECIFIC_OPS);

bool mlir_enabled()
{
#ifdef MIGRAPHX_MLIR
    const bool mlir_disabled = enabled(MIGRAPHX_DISABLE_MLIR{});
    return not mlir_disabled;
#else
    return false;
#endif
}

namespace {
struct requested
{
};
struct rejected
{
};
} // namespace

static bool is_negated_op(const std::string& s)
{
    if(s.empty())
        return false;
    return contains({'!', '~'}, s[0]);
}

template <class Action>
static std::vector<std::string> get_usage()
{
    static const auto options =
        split_string(string_value_of(MIGRAPHX_MLIR_USE_SPECIFIC_OPS{}, ""), ',');
    static const bool enabled = std::is_same<Action, requested>{};
    std::vector<std::string> result;
    auto remove_not_symbol = [&](const std::string& s) {
        if(is_negated_op(s))
            return s.substr(1);
        return s;
    };
    transform_if(
        options.begin(),
        options.end(),
        std::back_inserter(result),
        [&](const std::string& option) {
            if(option.empty())
                return false;
            if(is_negated_op(option))
                return not enabled;
            return enabled;
        },
        remove_not_symbol);
    return result;
}

template <class Action>
static bool specific_op(std::string_view option, bool fallback = false)
{
    static const auto options = get_usage<Action>();
    if(options.empty())
        return fallback;
    if(contains(option, "fused") and contains(options, "fused"))
        return true;
    return contains(options, option);
}

bool mlir_attention_enabled()
{
#ifdef MIGRAPHX_MLIR
    if(not mlir_enabled())
        return false;
    return specific_op<requested>("attention");
#else
    return false;
#endif
}

#ifdef MIGRAPHX_MLIR

struct mlir_op
{
    std::string name() const { return "gpu::mlir_op"; }
    operation op = make_op("convolution");

    template <class Self, class F>
    static auto reflect(Self& self, F f)
    {
        return pack(f(self.op, "op"));
    }

    shape compute_shape(const std::vector<shape>& inputs, const std::vector<module_ref>& mods) const
    {
        module_ref mod = mods[0];
        check_shapes{inputs, *this}.packed_or_broadcasted();
        if(mods.size() != 1)
            MIGRAPHX_THROW("should have one submodule.");
        if(inputs.size() < 2)
            MIGRAPHX_THROW("should have at least two inputs.");

        auto result =
            mod->compute_shapes(inputs, {.name = name(), .strict_type = true, .strict_lens = true});
        if(result.size() == 1)
            return result.front();
        return shape{result};
    }
};
MIGRAPHX_REGISTER_OP(mlir_op);

namespace {

const auto& reshaper_names()
{
    // clang-format off
    static const std::unordered_set<std::string> names = {
        "slice",
        "transpose",
        "multibroadcast",
        "broadcast",
        "contiguous",
        "reshape",
        "squeeze",
        "flatten",
        "unsqueeze"
    };
    // clang-format on
    return names;
}

std::tuple<instruction_ref, std::vector<operation>>
get_fusable_input_op_stream(instruction_ref lower_input)
{
    instruction_ref upper_input = lower_input;
    std::vector<operation> op_stream;
    while(contains(reshaper_names(), upper_input->name()))
    {
        operation op = upper_input->get_operator();
        if(contains({"squeeze", "flatten", "unsqueeze"}, upper_input->name()))
        {
            op = migraphx::make_op("reshape", {{"dims", upper_input->get_shape().lens()}});
        }
        op_stream.push_back(op);
        upper_input = upper_input->inputs().at(0);
    }
    return {upper_input, op_stream};
}

std::tuple<instruction_ref, std::vector<instruction_ref>>
fuse_input_ops_and_gemm_based_op(module_ref mm,
                                 const std::vector<instruction_ref>& gemm_based_op_inputs,
                                 const operation& gemm_based_op)
{
    std::vector<instruction_ref> top_inputs;
    std::vector<instruction_ref> imm_inputs;
    size_t input_cnt = 0;
    for(instruction_ref input : gemm_based_op_inputs)
    {
        auto [upper_input, op_stream] = get_fusable_input_op_stream(input);
        top_inputs.push_back(upper_input);
        instruction_ref prev_input =
            mm->add_parameter(param_name(input_cnt++, "y"), upper_input->get_shape().as_standard());
        for(const auto& op : reverse(op_stream))
        {
            prev_input = mm->add_instruction(op, {prev_input});
        }
        imm_inputs.push_back(prev_input);
    }
    instruction_ref new_gemm_based_op = mm->add_instruction(gemm_based_op, imm_inputs);
    return {new_gemm_based_op, top_inputs};
}

enum class mlir_mode
{
    all,
    fast,
    int8,
    none
};

auto is_mlir_dot(mlir_mode mode)
{
    return match::make_basic_pred_matcher([=](instruction_ref ins) {
        if(mode == mlir_mode::none)
            return false;
        if(ins->name() != "dot" and ins->name() != "quant_dot")
            return false;
        // dot operation where (FP8 * FP8 = FP8) is not available in MLIR. rocBLAS has the support
        // for it.
        if(ins->get_shape().type() == migraphx::shape::fp8e4m3fnuz_type)
            return false;
        if(mode != mlir_mode::fast)
            return true;
        auto a = ins->inputs().front()->get_shape();
        auto b = ins->inputs().back()->get_shape();
        // auto m = a.lens()[a.lens().size() - 2];
        // auto n = b.lens().back();
        auto k = a.lens().back();
        // Skipping GEMMs with a K dimension greater than 2048 is a course-grained strategy
        // to avoid poor-performing GEMM kernels from MLIR
        // To-do: Investigate a more precise strategy
        return k <= 1024;
    });
}

auto is_mlir_conv(mlir_mode mode)
{
    return match::make_basic_pred_matcher([=](instruction_ref ins) {
        if(mode == mlir_mode::none)
            return false;
        if(ins->name() != "convolution" and ins->name() != "quant_convolution")
            return false;
        auto input = ins->inputs().front()->get_shape();
        value v    = ins->get_operator().to_value();
        auto group = v.at("group").to<int>();
        // Avoid MLIR assertion: Index < Length && "Invalid index!"
        if(ins->get_shape().lens().size() != 4 and group > 1)
            return false;
        if(contains({shape::fp8e4m3fnuz_type, shape::int8_type}, input.type()))
            return true;
        if(mode == mlir_mode::all)
            return true;
        // No windograd for group convolution
        if(group > 1)
            return true;
        auto w = ins->inputs().at(1)->get_shape();
        if(w.lens().size() != 4)
            return true;
        if(w.lens()[2] != w.lens()[3])
            return true;
        return (w.lens()[3] % 3) != 0;
    });
}

std::unordered_map<instruction_ref, instruction_ref>
create_param_map_with_literals(module_ref mm, const module* pm, const shape& shape)
{
    std::unordered_map<instruction_ref, instruction_ref> ins_map;
    for(auto ins : iterator_for(*pm))
    {
        if(ins->name() != "@literal")
        {
            continue;
        }
        literal r               = ins->get_literal();
        instruction_ref literal = mm->add_literal(r);
        instruction_ref mbcast =
            mm->add_instruction(make_op("multibroadcast", {{"out_lens", shape.lens()}}), literal);
        ins_map[ins] = mbcast;
    }
    return ins_map;
}

// Whitelist supported fusion options, including imposing type constraints
// for cases where MLIR only supports an operation (usually a pointwise function)
// on particular types.
bool is_pointwise_op_supported_by_mlir(const instruction& i)
{
    using type_t                                      = shape::type_t;
    const auto& name                                  = i.name();
    const auto result_type                            = i.get_shape().type();
    const std::initializer_list<type_t> allowed_types = {type_t::float_type,
                                                         type_t::half_type,
                                                         type_t::fp8e4m3fnuz_type,
                                                         type_t::int8_type,
                                                         type_t::int32_type,
                                                         type_t::bool_type};
    // Preliminary type check.
    if(not contains(allowed_types, result_type))
    {
        return false;
    }
    const std::initializer_list<std::string> any_type_ops = {"@literal", "@param", "@return"};
    const std::initializer_list<std::string> no_bool_ops  = {
        "convolution",
        "quant_convolution",
        "dot",
        "quant_dot",
        "add",
        "clip",
        "relu",
        "sub",
        "mul",
        "div",
        "pow",
        "where",
        "quantizelinear",
        "dequantizelinear",
        "abs",
        "neg",
    };
    const std::initializer_list<std::string> fp_only_ops = {
        "ceil",
        "erf",
        "exp",
        "floor",
        "log",
        "recip",
        "sqrt",
        "rsqrt",
        "sigmoid",
        "softmax",
        "tanh",
    };
    bool is_float =
        contains({type_t::float_type, type_t::half_type, type_t::fp8e4m3fnuz_type}, result_type);
    if(contains(any_type_ops, name))
        return true;
    if(result_type != type_t::bool_type and contains(no_bool_ops, name))
        return true;
    if(is_float and contains(fp_only_ops, name))
        return true;
    // Only conversions between floating types are known to be unambigiously
    // supported.
    if(is_float and name == "convert")
    {
        if(result_type == shape::fp8e4m3fnuz_type)
        {
            return false;
        } // else
        return std::all_of(i.inputs().begin(), i.inputs().end(), [](const auto& arg) {
            return contains({type_t::float_type, type_t::half_type}, arg->get_shape().type());
        });
    }
    return false;
}

bool is_reduce_op_supported_by_mlir(const instruction& i)
{
    using type_t                                      = shape::type_t;
    const auto& name                                  = i.name();
    const auto result_type                            = i.get_shape().type();
    const std::initializer_list<type_t> allowed_types = {
        type_t::float_type, type_t::half_type, type_t::fp8e4m3fnuz_type};
    // Preliminary type check.
    if(not contains(allowed_types, result_type))
    {
        return false;
    }
    const std::initializer_list<std::string> reduce_ops = {"reduce_mean", "reduce_sum"};
    return contains(reduce_ops, i.name());
}

// A separate function so we can remove operators that are supported by mlir
// but not supported for an input fusion.
bool is_pointwise_op_supported_by_mlir_for_input(const instruction& i)
{
    return is_pointwise_op_supported_by_mlir(i);
}

MIGRAPHX_PRED_MATCHER(mlir_split_reduce, instruction_ref ins)
{
    if(ins->name() != "split_fused_reduce")
        return false;
    auto* mod_arg           = ins->module_inputs().front();
    auto supported_reshapes = reshaper_names();
    supported_reshapes.erase("slice");
    std::unordered_set<std::string> builtins = {"@param", "@literal", "@return"};
    for(const auto i : iterator_for(*mod_arg))
    {
        if(is_reduce(*i))
        {
            if(not is_reduce_op_supported_by_mlir(*i))
                return false;
        }
        else if(i->name() == "pointwise")
        {
            if(not std::all_of(i->module_inputs().front()->begin(),
                               i->module_inputs().front()->end(),
                               &is_pointwise_op_supported_by_mlir))
                return false;
        }
        else if(not contains(reshaper_names(), i->name()) and not contains(builtins, i->name()))
        {
            return false;
        }
    }
    return true;
}

MIGRAPHX_PRED_MATCHER(mlir_pointwise, instruction_ref ins)
{
    if(ins->name() != "pointwise")
        return false;
    auto* pm = ins->module_inputs().front();
    return std::all_of(pm->begin(), pm->end(), &is_pointwise_op_supported_by_mlir);
}

MIGRAPHX_PRED_MATCHER(mlir_input_pointwise, instruction_ref ins)
{
    if(ins->name() != "pointwise")
        return false;
    auto* pm = ins->module_inputs().front();
    return std::all_of(pm->begin(), pm->end(), &is_pointwise_op_supported_by_mlir_for_input);
}

std::vector<instruction_ref> mlir_contiguous(module_pass_manager& mpm,
                                             const std::vector<instruction_ref>& inputs)
{
    std::vector<instruction_ref> result;
    std::transform(
        inputs.begin(), inputs.end(), std::back_inserter(result), [&](instruction_ref input) {
            if(input->get_shape().packed() or input->get_shape().broadcasted())
                return input;
            return mpm.get_module().insert_instruction(
                std::next(input), make_op("contiguous"), input);
        });
    return result;
}

struct find_mlir_split_reduce
{
    mlir_mode conv_mode = mlir_mode::none;
    mlir_mode dot_mode  = mlir_mode::none;
    auto matcher() const
    {
        auto dot_or_conv = match::name("gpu::mlir_op");
        // TODO: Handle reshapes inbetween
        return mlir_split_reduce()(match::any_of[match::inputs()](dot_or_conv.bind("gemm")));
    }

    void apply(module_pass_manager& mpm, const match::matcher_result& r) const
    {
        auto reduce_ins = r.result;
        auto gemm_ins   = r.instructions["gemm"];
        assert(gemm_ins->get_shape().sub_shapes().empty());
        auto* rm   = reduce_ins->module_inputs().front();
        auto names = rm->get_parameter_names();
        std::sort(names.begin(), names.end());
        module_ref gemm_old_mm = gemm_ins->module_inputs().front();
        module_ref mm = mpm.create_module(gemm_old_mm->name() + "_" + rm->name(), *gemm_old_mm);
        // remove last return instruction
        if(std::prev(mm->end())->name() == "@return")
        {
            mm->remove_instruction(std::prev(mm->end()));
        }
        mm->set_bypass();
        std::unordered_map<instruction_ref, instruction_ref> param_map;
        param_map[gemm_ins]      = std::prev(mm->end());
        bool gemm_has_multi_outs = gemm_ins->outputs().size() > 1;
        auto return_vals =
            mm->fuse(*rm,
                     reduce_ins->inputs(),
                     &param_map,
                     [&](module& main_mod,
                         instruction_ref pos,
                         const operation& op,
                         const std::vector<instruction_ref>& inputs,
                         const std::vector<module_ref>& mod_args) {
                         if(op.name() == "pointwise")
                         {
                             auto* sub_pm     = mod_args.front();
                             auto param_map_2 = create_param_map_with_literals(
                                 &main_mod, sub_pm, op.compute_shape(to_shapes(inputs), mod_args));
                             return main_mod.insert_inline(pos, *sub_pm, inputs, &param_map_2)
                                 .front(); // cppcheck-suppress returnDanglingLifetime;
                         }
                         return main_mod.insert_instruction(pos, op, inputs, mod_args);
                     });
        if(gemm_has_multi_outs)
        {
            return_vals.insert(return_vals.end(), param_map[gemm_ins]);
        }
        mm->add_return(return_vals);
        std::vector<instruction_ref> inputs;
        std::copy_if(reduce_ins->inputs().begin(),
                     reduce_ins->inputs().end(),
                     std::back_inserter(inputs),
                     [&](auto input) { return input != gemm_ins; });
        inputs.insert(inputs.end(), gemm_ins->inputs().begin(), gemm_ins->inputs().end());
        if(gemm_has_multi_outs)
        {
            auto fused_ins = mpm.get_module().insert_instruction(
                reduce_ins, mlir_op{gemm_ins->get_operator()}, mlir_contiguous(mpm, inputs), {mm});
            auto dot_ins = mpm.get_module().insert_instruction(
                reduce_ins,
                migraphx::make_op("get_tuple_elem", {{"index", return_vals.size() - 1}}),
                fused_ins);

            mpm.get_module().replace_instruction(gemm_ins, dot_ins);
            for(const auto& outs : reduce_ins->outputs())
            {
                assert(outs->get_operator().name() == "get_tuple_elem");
                mpm.get_module().replace_instruction(outs, outs->get_operator(), fused_ins);
            }
        }
        else
        {
            mpm.get_module().replace_instruction(
                reduce_ins, mlir_op{gemm_ins->get_operator()}, mlir_contiguous(mpm, inputs), {mm});
        }
    }
};

struct find_mlir_fused_ops
{
    mlir_mode conv_mode = mlir_mode::none;
    mlir_mode dot_mode  = mlir_mode::none;
    auto matcher() const
    {
        auto reshapes = reshaper_names();
        // slice is not supported
        reshapes.erase("slice");
        auto dot_or_conv = match::skip(match::name(reshapes))(
            match::any_of(is_mlir_dot(dot_mode), is_mlir_conv(conv_mode)).bind("gemm_based_op"));
        return mlir_pointwise()(match::any_of[match::inputs()](dot_or_conv.bind("x")));
    }

    void apply(module_pass_manager& mpm, const match::matcher_result& r) const
    {
        auto pw_ins        = r.result;
        auto gemm_based_op = r.instructions["gemm_based_op"];
        auto x_ins         = r.instructions["x"]; // input to pointwise after reshaper op stream
        auto* pm           = pw_ins->module_inputs().front();
        auto pw_inputs     = pw_ins->inputs();
        // only of one of the inputs to pointwise module should be dependent on conv/gemm that is
        // being fused, otherwise it can create invalid graph transformation
        if(std::any_of(pw_inputs.begin(), pw_inputs.end(), [&](const auto& i) {
               return i != x_ins and reaches(gemm_based_op, i);
           }))
            return;
        auto names = pm->get_parameter_names();
        std::sort(names.begin(), names.end());
        module_ref mm = mpm.create_module("mlir_" + pm->name());
        mm->set_bypass();
        auto [anchor_op, top_inputs] = fuse_input_ops_and_gemm_based_op(
            mm, gemm_based_op->inputs(), gemm_based_op->get_operator());
        std::unordered_map<instruction_ref, instruction_ref> param_map =
            create_param_map_with_literals(mm, pm, pw_ins->get_shape());
        auto [upper_input, op_stream] = get_fusable_input_op_stream(x_ins);
        assert(upper_input == gemm_based_op);
        auto prev_input = anchor_op;
        for(const auto& op : reverse(op_stream))
        {
            prev_input = mm->add_instruction(op, {prev_input});
        }
        assert(prev_input->get_shape().lens() == x_ins->get_shape().lens());
        param_map[x_ins] = prev_input; // this is to avoid adding parameter for gemm/conv reshaped
                                       // input to pointwise in new fused module
        bool gemm_has_multi_outs = gemm_based_op->outputs().size() > 1;
        auto reshaped_gemm       = x_ins;
        std::vector<instruction_ref> reshapes_vec;
        while(reshaped_gemm != gemm_based_op)
        {
            reshapes_vec.push_back(reshaped_gemm);
            gemm_has_multi_outs = gemm_has_multi_outs or reshaped_gemm->outputs().size() > 1;
            reshaped_gemm       = reshaped_gemm->inputs().at(0);
        }
        reshapes_vec.push_back(reshaped_gemm);

        auto return_vals = mm->fuse(*pm, pw_ins->inputs(), &param_map);
        if(gemm_has_multi_outs)
        {
            return_vals.insert(return_vals.begin(), anchor_op);
        }
        mm->add_return(return_vals);

        std::vector<instruction_ref> inputs;
        std::copy_if(pw_ins->inputs().begin(),
                     pw_ins->inputs().end(),
                     std::back_inserter(inputs),
                     [&](auto input) { return input != x_ins; });
        inputs.insert(inputs.end(), top_inputs.begin(), top_inputs.end());
        if(gemm_has_multi_outs)
        {
            auto fused_ins = mpm.get_module().insert_instruction(
                pw_ins, mlir_op{gemm_based_op->get_operator()}, mlir_contiguous(mpm, inputs), {mm});
            mpm.get_module().replace_instruction(
                pw_ins, migraphx::make_op("get_tuple_elem", {{"index", 1}}), fused_ins);
            auto dot_ins = mpm.get_module().insert_instruction(
                pw_ins, migraphx::make_op("get_tuple_elem", {{"index", 0}}), fused_ins);
            // move all the reshape instructions and original GEMM instruction after the fused op to
            // avoid generating invalid migraphx program
            for(const auto& orig_i : reverse(reshapes_vec))
            {
                mpm.get_module().move_instruction(orig_i, pw_ins);
            }
            mpm.get_module().replace_instruction(gemm_based_op, dot_ins);
        }
        else
        {
            mpm.get_module().replace_instruction(
                pw_ins, mlir_op{gemm_based_op->get_operator()}, mlir_contiguous(mpm, inputs), {mm});
        }
    }
};

template <auto Matcher>
struct find_mlir_standalone_op
{
    mlir_mode mode = mlir_mode::none;
    auto matcher() const { return Matcher(mode); }

    void apply(module_pass_manager& mpm, const match::matcher_result& r) const
    {
        auto gemm_based_op = r.result;
        // enable only for fp32/fp16/i8/fp8 types
        if(std::any_of(gemm_based_op->inputs().begin(), gemm_based_op->inputs().end(), [&](auto i) {
               return not contains({shape::type_t::float_type,
                                    shape::type_t::half_type,
                                    shape::type_t::int8_type,
                                    shape::type_t::fp8e4m3fnuz_type},
                                   i->get_shape().type());
           }))
            return;
        static size_t counter = 0;
        module_ref mm =
            mpm.create_module("mlir_" + gemm_based_op->name() + std::to_string(counter++));
        mm->set_bypass();
        auto [anchor_op, top_inputs] = fuse_input_ops_and_gemm_based_op(
            mm, gemm_based_op->inputs(), gemm_based_op->get_operator());
        mm->add_return({anchor_op});
        mpm.get_module().replace_instruction(gemm_based_op,
                                             mlir_op{gemm_based_op->get_operator()},
                                             mlir_contiguous(mpm, top_inputs),
                                             {mm});
    }
};

using find_mlir_standalone_convolution_op = find_mlir_standalone_op<&is_mlir_conv>;
using find_mlir_standalone_dot_op         = find_mlir_standalone_op<&is_mlir_dot>;

struct find_mlir_standalone_attention_op
{
    auto matcher() const
    {
        return match::name("gpu::pre_gemm_softmax_gemm").bind("gemm_softmax_gemm");
    }

    void apply(module_pass_manager& mpm, const match::matcher_result& r) const
    {
        static size_t counter  = 0;
        module_ref mm          = mpm.create_module("mlir_" + std::to_string(counter++));
        auto gemm_softmax_gemm = r.instructions["gemm_softmax_gemm"];
        mm->set_bypass();

        auto orig_inputs = gemm_softmax_gemm->inputs();

        std::vector<instruction_ref> gemm0_inputs = {orig_inputs[0], orig_inputs[1]};
        auto [gemm0, top_gemm0_inputs] =
            fuse_input_ops_and_gemm_based_op(mm, gemm0_inputs, make_op("dot"));

        std::vector<instruction_ref> inputs;
        inputs.insert(inputs.begin(), top_gemm0_inputs.begin(), top_gemm0_inputs.end());

        // handle scale
        auto v = gemm_softmax_gemm->get_operator().to_value();
        assert(v.contains("scale"));
        auto scale     = v.at("scale").to<float>();
        auto scale_lit = mm->add_literal(literal{shape{gemm0->get_shape().type()}, {scale}});
        instruction_ref scale_lit_mbcast = mm->add_instruction(
            make_op("multibroadcast", {{"out_lens", gemm0->get_shape().lens()}}), scale_lit);
        auto scaled_gemm0 = mm->add_instruction(make_op("mul"), gemm0, scale_lit_mbcast);

        std::optional<instruction_ref> bias{nullopt};
        if(orig_inputs.size() == 4)
        {
            auto bias_input = orig_inputs[2];
            instruction_ref bias_param =
                mm->add_parameter("y_bias", bias_input->get_shape().as_standard());
            bias = mm->add_instruction(make_op("add"), scaled_gemm0, bias_param);
            inputs.push_back(bias_input);
        }
        else if(orig_inputs.size() == 5) // gemm1 + mul_where + softmax + gemm2 + trailing_pm case
        {
            auto select_cond  = orig_inputs[2];
            auto select_const = orig_inputs[3];
            instruction_ref select_cond_param =
                mm->add_parameter("y_cond", select_cond->get_shape().as_standard());
            instruction_ref select_cond_const =
                mm->add_parameter("y_const", select_const->get_shape().as_standard());
            bias = mm->add_instruction(
                make_op("where"), select_cond_param, scaled_gemm0, select_cond_const);
            inputs.push_back(select_cond);
            inputs.push_back(select_const);
        }

        auto softmax = mm->add_instruction(
            make_op("softmax", {{"axis", gemm0->get_shape().lens().size() - 1}}),
            bias ? bias.value() : scaled_gemm0);
        auto [old_upper_v, upper_v_op_stream] = get_fusable_input_op_stream(orig_inputs.back());
        instruction_ref new_upper_v =
            mm->add_parameter("z", old_upper_v->get_shape().as_standard());
        for(const auto& op : reverse(upper_v_op_stream))
        {
            new_upper_v = mm->add_instruction(op, {new_upper_v});
        }
        inputs.push_back(old_upper_v);

        auto gemm1 = mm->add_instruction(make_op("dot"), {softmax, new_upper_v});

        std::vector<instruction_ref> ins_to_replace = {gemm1};
        auto ins_to_be_replaced                     = gemm_softmax_gemm;
        if(r.instructions.find("trailing_pm") != r.instructions.end())
        {
            auto trailing_pm_ins = r.instructions["trailing_pm"];
            auto ins_map         = create_param_map_with_literals(
                mm, trailing_pm_ins->module_inputs().front(), trailing_pm_ins->get_shape());
            ins_map[gemm_softmax_gemm] = gemm1;
            ins_to_replace             = mm->fuse(
                *trailing_pm_ins->module_inputs().front(), trailing_pm_ins->inputs(), &ins_map);
            std::copy_if(trailing_pm_ins->inputs().begin(),
                         trailing_pm_ins->inputs().end(),
                         std::back_inserter(inputs),
                         [&](auto input) { return input != gemm_softmax_gemm; });
            ins_to_be_replaced = trailing_pm_ins;
        }
        mm->add_return(ins_to_replace);

        mpm.get_module().replace_instruction(
            ins_to_be_replaced, mlir_op{gemm1->get_operator()}, mlir_contiguous(mpm, inputs), {mm});
    }
};

struct find_mlir_attention_fused_ops : public find_mlir_standalone_attention_op
{
    auto matcher() const
    {
        auto standalone_matcher = find_mlir_standalone_attention_op::matcher();
        return mlir_pointwise()(
            match::any_of[match::inputs()](standalone_matcher).bind("trailing_pm"));
        ;
    }
};

struct find_pointwise_mlir
{
    auto matcher() const
    {
        return match::name("gpu::mlir_op")(match::any_of[match::inputs()](
            mlir_input_pointwise(match::used_once()).bind("pointwise")));
    }

    static instruction_ref insert_pointwise(module& m,
                                            instruction_ref ins,
                                            const operation& op,
                                            const std::vector<instruction_ref>& inputs,
                                            const std::vector<module_ref>& mod_args)
    {
        // Only used in assert
        (void)mod_args;
        assert(mod_args.empty());
        return insert_common_op(m, ins, op, inputs);
    }

    void apply(module_pass_manager& mpm, const match::matcher_result& r) const
    {
        auto ins = r.result;
        auto pw  = r.instructions["pointwise"];

        auto* mm = ins->module_inputs().front();
        auto* pm = pw->module_inputs().front();

        std::unordered_map<instruction_ref, instruction_ref> map_ins;
        module_ref m = mpm.create_module(pm->name() + ":" + mm->name());
        m->set_bypass();
        auto rins   = m->fuse(*pm, pw->inputs(), &map_ins, &insert_pointwise).front();
        map_ins[pw] = rins;

        auto ret = m->fuse(*mm, ins->inputs(), &map_ins);
        m->add_return({ret});

        auto inputs = find_inputs(map_ins, &mpm.get_module(), m);
        mpm.get_module().replace_instruction(
            ins, ins->get_operator(), mlir_contiguous(mpm, inputs), {m});
    }
};

} // namespace

#endif // MIGRAPHX_MLIR

void fuse_mlir::apply(module_pass_manager& mpm) const
{
#ifdef MIGRAPHX_MLIR
    const auto& device_name = ctx == nullptr ? "" : ctx->get_current_device().get_gfx_name();
    const bool is_navi      = starts_with(device_name, "gfx11");

    auto get_mode = [&](std::string_view option, mlir_mode m1, mlir_mode m2 = mlir_mode::fast) {
        if(specific_op<rejected>(option))
            return mlir_mode::none;
        if(specific_op<requested>(option))
            return mlir_mode::all;
        if(is_navi)
            return mlir_mode::all;
        return std::max(m1, m2);
    };

    // Attention offloads; default disabled
    if(mlir_attention_enabled())
    {
        match::find_matches(mpm, find_mlir_attention_fused_ops{});
        match::find_matches(mpm, find_mlir_standalone_attention_op{});
    }

    match::find_matches(
        mpm,
        find_mlir_fused_ops{.conv_mode = get_mode("fused_convolution", mlir_mode::fast),
                            .dot_mode  = get_mode("fused_dot", mlir_mode::fast)});

    match::find_matches(
        mpm,
        find_mlir_standalone_convolution_op{get_mode("convolution", mlir_mode::fast)},
        find_mlir_standalone_dot_op{get_mode("dot", mlir_mode::fast)});

    mpm.run_pass(dead_code_elimination{});
    if(enabled(MIGRAPHX_ENABLE_MLIR_REDUCE_FUSION{}))
    {
        match::find_matches(
            mpm,
            find_mlir_split_reduce{.conv_mode = get_mode("fused_convolution", mlir_mode::fast),
                                   .dot_mode  = get_mode("fused_dot", mlir_mode::fast)});
    }

    if(enabled(MIGRAPHX_ENABLE_MLIR_INPUT_FUSION{}))
    {
        match::find_matches(mpm, find_pointwise_mlir{});
    }
#else
    (void)mpm;
#endif
}

} // namespace gpu
} // namespace MIGRAPHX_INLINE_NS
} // namespace migraphx
