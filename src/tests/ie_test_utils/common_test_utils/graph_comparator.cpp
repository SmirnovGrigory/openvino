// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "common_test_utils/graph_comparator.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <openvino/op/util/op_types.hpp>
#include <openvino/op/util/sub_graph_base.hpp>
#include <openvino/opsets/opset8.hpp>
#include <queue>
#include <sstream>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <vector>
#include "ov_tensor_utils.hpp"
#include "ngraph_functions/utils/ngraph_helpers.hpp"

namespace {
inline namespace tools {
bool is_type_relaxed(const std::string& type) {
    return type.find_first_of("TypeRelaxed") == 0;
}

bool compare_type_info(const ngraph::DiscreteTypeInfo& info1, const ngraph::DiscreteTypeInfo& info2) {
    OPENVINO_SUPPRESS_DEPRECATED_START
    if (!is_type_relaxed(info1.name) && !is_type_relaxed(info2.name) && (info1.version != info2.version)) {
        return false;
    }
    OPENVINO_SUPPRESS_DEPRECATED_END

    const std::string info1Name =
        is_type_relaxed(info1.name) && (info1.parent != nullptr) ? info1.parent->name : info1.name;
    const std::string info2Name =
        is_type_relaxed(info2.name) && (info2.parent != nullptr) ? info2.parent->name : info2.name;
    return info1Name == info2Name;
}

template <typename T>
bool compare_rt_keys(const T& node1, const T& node2, std::ostream& err_log) {
    const auto& first_node_rt_info = node1.get_rt_info();
    const auto& second_node_rt_info = node2.get_rt_info();

    for (const auto& attr : second_node_rt_info) {
        const auto& key = attr.first;
        // TODO: remove this check after 77716 is implemented
        if (key == "opset") {
            continue;
        }
        auto value1 = first_node_rt_info.find(key);
        if (value1 == first_node_rt_info.end()) {
            err_log << "Key: " << key << " is missing.\n";
            return false;
        }
        try {
            if (value1->second != attr.second) {
                err_log << "Values for " << key << " key are not equal.\n";
                return false;
            }
        } catch (ov::Exception& e) {
            // Handle cases wen equality operator is not defined for some rt attribute
        }
    }
    return true;
}

bool less_by_name(const std::shared_ptr<ngraph::op::v0::Result>& l, const std::shared_ptr<ngraph::op::v0::Result>& r) {
    return l->get_friendly_name() < r->get_friendly_name();
}

bool less_by_parent_name(const std::shared_ptr<ngraph::op::v0::Result>& l,
                         const std::shared_ptr<ngraph::op::v0::Result>& r) {
    return l->get_input_node_shared_ptr(0)->get_friendly_name() < r->get_input_node_shared_ptr(0)->get_friendly_name();
}

std::string typeInfoToStr(const ngraph::Node::type_info_t& typeInfo) {
    OPENVINO_SUPPRESS_DEPRECATED_START
    return std::string(typeInfo.name) + "/" + to_str(typeInfo.version);
    OPENVINO_SUPPRESS_DEPRECATED_END
}

std::string tensor_names(const ngraph::descriptor::Tensor& t) {
    std::string n;
    const char* glue = "";
    for (const auto& name : t.get_names()) {
        n.append(glue).append(name);
        glue = ", ";
    }
    return "\"" + n + "\"";
}
}  // namespace tools

namespace subgraph {

namespace detail {

template <typename Ptr>
Ptr not_null(Ptr&& p) {
    if (!p) {
        throw ov::Exception("empty pointer");
    }
    return std::forward<Ptr>(p);
}

template <typename InOut1, typename InOut2>
bool equal_type_and_partial_shape(const InOut1& lhs, const InOut2& rhs) {
    return lhs.get_element_type() == rhs.get_element_type() && lhs.get_partial_shape() == rhs.get_partial_shape();
}

class NodeAndInputDescription {
public:
    using SubGraphOp = ov::op::util::SubGraphOp;
    using InputDescripton = SubGraphOp::InputDescription;
    using InputNode = ngraph::Input<ngraph::Node>;
    using Parameter = ov::opset8::Parameter;

    explicit NodeAndInputDescription(const InputNode& input,
                                     const Parameter* parameter,
                                     const InputDescripton* description)
        : m_input(input),
          m_parameter(not_null(parameter)),
          m_description(not_null(description)) {}

    static bool equal_descriptions(const InputDescripton* lhs, const InputDescripton* rhs) {
        if (!lhs || !rhs || lhs->get_type_info() != rhs->get_type_info()) {
            return false;
        }

        if (lhs->get_type_info() == SubGraphOp::SliceInputDescription::get_type_info_static()) {
            using InDesc = SubGraphOp::SliceInputDescription;
            const auto* l_input = static_cast<const InDesc*>(lhs);
            const auto* r_input = static_cast<const InDesc*>(rhs);
            return l_input->m_start == r_input->m_start && l_input->m_stride == r_input->m_stride &&
                   l_input->m_part_size == r_input->m_part_size && l_input->m_end == r_input->m_end &&
                   l_input->m_axis == r_input->m_axis;
        } else if (lhs->get_type_info() == SubGraphOp::MergedInputDescription::get_type_info_static()) {
            return true;  // noting extra to check
        } else if (lhs->get_type_info() == SubGraphOp::InvariantInputDescription::get_type_info_static()) {
            return true;  // noting extra to check
        }

        std::stringstream ss;
        ss << "Type is not supported: [" << lhs->get_type_info().name << "]";
        throw ov::Exception(ss.str());
    }

    bool parameter_and_input_match(size_t num_iterations) const {
        if (const SubGraphOp::SliceInputDescription* slice_description =
                ngraph::as_type<const SubGraphOp::SliceInputDescription>(m_description)) {
            if (m_parameter->get_element_type() != m_input.get_element_type()) {
                return false;
            }
            const auto& param_partial_shape = m_parameter->get_partial_shape();
            const auto& input_partial_shape = m_input.get_partial_shape();
            if (param_partial_shape.is_dynamic() && input_partial_shape.is_dynamic()) {
                return true;
            }
            if (!param_partial_shape.is_static() || !input_partial_shape.is_static()) {
                return false;
            }
            const auto& param_shape = param_partial_shape.to_shape();
            const auto& input_shape = input_partial_shape.to_shape();
            if (param_shape.size() != input_shape.size()) {
                return false;
            }
            if (param_shape[slice_description->m_axis] != slice_description->m_part_size) {
                return false;
            }
            for (size_t i = 0; i != param_shape.size(); ++i) {
                const auto expected_axis_size =
                    i == slice_description->m_axis ? slice_description->m_part_size * num_iterations : param_shape[i];
                if (input_shape[i] != expected_axis_size) {
                    return false;
                }
            }
            return true;
        } else if (m_description->get_type_info() == SubGraphOp::MergedInputDescription::get_type_info_static() ||
                   m_description->get_type_info() == SubGraphOp::InvariantInputDescription::get_type_info_static()) {
            return equal_type_and_partial_shape(*m_parameter, m_input);
        }

        std::stringstream ss;
        ss << "Type is not supported: [" << m_description->get_type_info().name << "]";
        throw ov::Exception(ss.str());
    }

    static bool equal_parameters(const Parameter* lhs, const Parameter* rhs) {
        return lhs && rhs && equal_type_and_partial_shape(*lhs, *rhs);
    }

    friend bool operator==(const NodeAndInputDescription& lhs, const NodeAndInputDescription& rhs) {
        if (!equal_descriptions(lhs.m_description, rhs.m_description)) {
            return false;
        }
        return equal_parameters(lhs.m_parameter, rhs.m_parameter);
    }

private:
    const InputNode m_input;
    const Parameter* m_parameter;
    const InputDescripton* m_description;
};

class NodeAndOutputDescription {
public:
    using SubGraphOp = ov::op::util::SubGraphOp;
    using OutputDescription = SubGraphOp::OutputDescription;
    using OutputNode = ngraph::Output<ngraph::Node>;
    using Result = ov::opset8::Result;

    explicit NodeAndOutputDescription(const OutputNode& output,
                                      const Result* result,
                                      const OutputDescription* description)
        : m_output(output),
          m_result(not_null(result)),
          m_description(not_null(description)) {}

    static bool equal_descriptions(const OutputDescription* lhs, const OutputDescription* rhs) {
        if (!lhs || !rhs || lhs->get_type_info() != rhs->get_type_info()) {
            return false;
        }

        if (lhs->get_type_info() == SubGraphOp::ConcatOutputDescription::get_type_info_static()) {
            using OutDesc = SubGraphOp::ConcatOutputDescription;
            const auto* l_output = static_cast<const OutDesc*>(lhs);
            const auto* r_output = static_cast<const OutDesc*>(rhs);
            return l_output->m_start == r_output->m_start && l_output->m_stride == r_output->m_stride &&
                   l_output->m_part_size == r_output->m_part_size && l_output->m_end == r_output->m_end &&
                   l_output->m_axis == r_output->m_axis;
        } else if (lhs->get_type_info() == SubGraphOp::BodyOutputDescription::get_type_info_static()) {
            using OutDesc = SubGraphOp::BodyOutputDescription;
            const auto* l_output = static_cast<const OutDesc*>(lhs);
            const auto* r_output = static_cast<const OutDesc*>(rhs);
            return l_output->m_iteration == r_output->m_iteration;
        }

        std::stringstream ss;
        ss << "Type is not supported: [" << lhs->get_type_info().name << "]";
        throw ov::Exception(ss.str());
    }

    bool result_and_output_match(size_t num_iterations) const {
        if (const auto concat_desciption = ngraph::as_type<const SubGraphOp::ConcatOutputDescription>(m_description)) {
            if (m_result->output(0).get_element_type() != m_output.get_element_type()) {
                return false;
            }

            const auto& output_partial_shape = m_output.get_partial_shape();
            const auto& result_partial_shape = m_result->output(0).get_partial_shape();
            if (result_partial_shape.is_dynamic() && output_partial_shape.is_dynamic()) {
                return true;
            }
            if (!result_partial_shape.is_static() || !output_partial_shape.is_static()) {
                return false;
            }
            const auto& output_shape = output_partial_shape.to_shape();
            const auto& result_shape = result_partial_shape.to_shape();
            if (result_shape.size() != output_shape.size()) {
                return false;
            }
            for (size_t i = 0; i != result_shape.size(); ++i) {
                const auto axis_multiplier = i == concat_desciption->m_axis ? num_iterations : 1;
                if (result_shape[i] * axis_multiplier != output_shape[i]) {
                    return false;
                }
            }
            return true;
        } else if (m_description->get_type_info() == SubGraphOp::BodyOutputDescription::get_type_info_static()) {
            return equal_type_and_partial_shape(m_result->output(0), m_output);
        }

        std::stringstream ss;
        ss << "Type is not supported: [" << m_description->get_type_info().name << "]";
        throw ov::Exception(ss.str());
    }

    static bool equal_results(const Result* lhs, const Result* rhs) {
        return lhs && rhs && equal_type_and_partial_shape(lhs->output(0), rhs->output(0));
    }

    friend bool operator==(const NodeAndOutputDescription& lhs, const NodeAndOutputDescription& rhs) {
        if (!equal_descriptions(lhs.m_description, rhs.m_description)) {
            return false;
        }
        return equal_results(lhs.m_result, rhs.m_result);
    }

private:
    const OutputNode m_output;
    const Result* m_result;
    const OutputDescription* m_description;
};

class BackEdge {
public:
    using Parameter = ov::opset8::Parameter;
    using Result = ov::opset8::Result;
    using Id = uint64_t;

    explicit BackEdge(const Parameter* parameter, const Result* result)
        : m_parameter(not_null(parameter)),
          m_result(not_null(result)) {}

    bool result_and_parameter_match() const {
        return equal_type_and_partial_shape(m_result->output(0), *m_parameter);
    }

    friend bool operator==(const BackEdge& lhs, const BackEdge& rhs) {
        return equal_type_and_partial_shape(*lhs.m_parameter, *rhs.m_parameter) &&
               equal_type_and_partial_shape(lhs.m_result->output(0), rhs.m_result->output(0));
    }

private:
    const Parameter* m_parameter;
    const Result* m_result;
};

std::vector<NodeAndInputDescription> extract_inputs(ov::op::util::SubGraphOp* sub) {
    std::vector<NodeAndInputDescription> nodes;
    const auto& fn_body = sub->get_function();
    const auto& fn_parameters = fn_body->get_parameters();

    for (const auto& in_desc : sub->get_input_descriptions()) {
        const auto parameter = fn_parameters.at(in_desc->m_body_parameter_index).get();
        const auto input = sub->input(in_desc->m_input_index);
        nodes.emplace_back(input, parameter, in_desc.get());
    }
    return nodes;
}

std::vector<NodeAndOutputDescription> extract_outputs(ov::op::util::SubGraphOp* sub) {
    std::vector<NodeAndOutputDescription> nodes;
    const auto& fn_body = sub->get_function();
    const auto& fs_results = fn_body->get_results();

    for (const auto& out_desc : sub->get_output_descriptions()) {
        const auto result = fs_results.at(out_desc->m_body_value_index).get();
        const auto output = sub->output(out_desc->m_output_index);
        nodes.emplace_back(output, result, out_desc.get());
    }
    return nodes;
}

std::vector<BackEdge> extract_backedges(ov::op::util::SubGraphOp* sub) {
    using MergedInputDescription = ov::op::util::SubGraphOp::MergedInputDescription;
    std::vector<BackEdge> edges;
    const auto& fn_body = sub->get_function();

    const auto& fs_parameters = fn_body->get_parameters();
    const auto& fs_results = fn_body->get_results();

    for (const auto& in_desc : sub->get_input_descriptions()) {
        if (const auto& merged_in_desc = ngraph::as_type_ptr<const MergedInputDescription>(in_desc)) {
            const auto parameter = fs_parameters.at(merged_in_desc->m_body_parameter_index);
            const auto result = fs_results.at(merged_in_desc->m_body_value_index);
            edges.emplace_back(parameter.get(), result.get());
        }
    }
    return edges;
}

struct NotValidInputOrOutput {
    NotValidInputOrOutput(int64_t num_iterations) : m_num_iterations(num_iterations) {}

    bool operator()(const NodeAndOutputDescription& d) const {
        return !d.result_and_output_match(m_num_iterations);
    }

    bool operator()(const NodeAndInputDescription& d) const {
        return !d.parameter_and_input_match(m_num_iterations);
    }

    int64_t m_num_iterations;
};

bool not_valid_back_edge(const BackEdge& be) {
    return !be.result_and_parameter_match();
}

bool equal_body_ports(ov::opset8::Loop* lhs, ov::opset8::Loop* rhs) {
    if (!lhs || !rhs) {
        return false;
    }
    const auto& lhs_fn_body = lhs->get_function();
    const auto& rhs_fn_body = rhs->get_function();

    const auto& lhs_sbp = lhs->get_special_body_ports();
    const auto& rhs_sbp = rhs->get_special_body_ports();

    constexpr int64_t port_not_provided = -1;

    const bool input_provided = lhs_sbp.current_iteration_input_idx != port_not_provided ||
                                rhs_sbp.current_iteration_input_idx != port_not_provided;

    if (input_provided) {
        const auto& lhs_parameter = lhs_fn_body->get_parameters().at(lhs_sbp.current_iteration_input_idx);
        const auto& rhs_parameter = rhs_fn_body->get_parameters().at(rhs_sbp.current_iteration_input_idx);
        if (!NodeAndInputDescription::equal_parameters(lhs_parameter.get(), rhs_parameter.get())) {
            return false;
        }
    }

    const auto& lhs_result = lhs_fn_body->get_results().at(lhs_sbp.body_condition_output_idx);
    const auto& rhs_result = rhs_fn_body->get_results().at(rhs_sbp.body_condition_output_idx);

    return NodeAndOutputDescription::equal_results(lhs_result.get(), rhs_result.get());
}

class CompareSubGraphs {
public:
    using Result = Comparator::Result;
    using SubGraphOp = ov::op::util::SubGraphOp;

    Result compare(SubGraphOp* sub_lhs, SubGraphOp* sub_rhs) {
        const auto lhs_it_no = get_num_iterations(sub_lhs);
        const auto rhs_it_no = get_num_iterations(sub_rhs);
        if (lhs_it_no != rhs_it_no) {
            return Result::error("different number of iterations");
        }

        not_valid_input_output = lhs_it_no;

        const auto result_for_inputs = compare_inputs(sub_lhs, sub_rhs);
        if (!result_for_inputs.valid) {
            return result_for_inputs;
        }

        const auto result_for_outputs = compare_outputs(sub_lhs, sub_rhs);
        if (!result_for_outputs.valid) {
            return result_for_outputs;
        }

        return compare_backedges(sub_lhs, sub_rhs);
    }

private:
    Result compare_inputs(SubGraphOp* sub_lhs, SubGraphOp* sub_rhs) const {
        const auto& lhs_sub_inputs = extract_inputs(sub_lhs);
        const auto& rhs_sub_inputs = extract_inputs(sub_rhs);

        if (lhs_sub_inputs.empty() || rhs_sub_inputs.empty()) {
            return Result::error("no input in subgraph");
        }

        if (std::any_of(begin(lhs_sub_inputs), end(lhs_sub_inputs), not_valid_input_output)) {
            return Result::error("inputs and parameters mismatch");
        }
        if (std::any_of(begin(rhs_sub_inputs), end(rhs_sub_inputs), not_valid_input_output)) {
            return Result::error("inputs and parameters mismatch");
        }

        if (lhs_sub_inputs.size() != rhs_sub_inputs.size() ||
            !std::is_permutation(begin(lhs_sub_inputs), end(lhs_sub_inputs), begin(rhs_sub_inputs))) {
            return Result::error("different SubGraph InputDescription");
        }
        return Result::ok();
    }

    Result compare_outputs(SubGraphOp* sub_lhs, SubGraphOp* sub_rhs) const {
        const auto& lhs_sub_outputs = extract_outputs(sub_lhs);
        const auto& rhs_sub_outputs = extract_outputs(sub_rhs);

        if (lhs_sub_outputs.empty() || rhs_sub_outputs.empty()) {
            return Result::error("no output in subgraph");
        }

        if (std::any_of(begin(lhs_sub_outputs), end(lhs_sub_outputs), not_valid_input_output)) {
            return Result::error("outputs and results mismatch");
        }
        if (std::any_of(begin(rhs_sub_outputs), end(rhs_sub_outputs), not_valid_input_output)) {
            return Result::error("outputs and results mismatch");
        }

        if (lhs_sub_outputs.size() != rhs_sub_outputs.size() ||
            !std::is_permutation(begin(lhs_sub_outputs), end(lhs_sub_outputs), begin(rhs_sub_outputs))) {
            return Result::error("different SubGraph OutputDescription");
        }
        return Result::ok();
    }

    Result compare_backedges(SubGraphOp* sub_lhs, SubGraphOp* sub_rhs) const {
        const auto lhs_back_edges = extract_backedges(sub_lhs);
        const auto rhs_back_edges = extract_backedges(sub_rhs);

        if (std::any_of(begin(lhs_back_edges), end(lhs_back_edges), not_valid_back_edge)) {
            return Result::error("back edges mismatch");
        }
        if (std::any_of(begin(rhs_back_edges), end(rhs_back_edges), not_valid_back_edge)) {
            return Result::error("back edges mismatch");
        }

        if (lhs_back_edges.size() != rhs_back_edges.size() ||
            !std::is_permutation(begin(lhs_back_edges), end(lhs_back_edges), begin(rhs_back_edges))) {
            return Result::error("different SubGraph BackEdges");
        }
        if (auto loop_lhs = ngraph::as_type<ov::opset8::Loop>(sub_lhs)) {
            auto loop_rhs = ngraph::as_type<ov::opset8::Loop>(sub_rhs);
            if (!equal_body_ports(loop_lhs, loop_rhs)) {
                return Result::error("different Special Body Ports");
            }
        }
        return Result::ok();
    }

    static int64_t get_num_iterations(ov::op::util::SubGraphOp* sub) {
        using namespace ov::opset8;
        if (const auto ti = dynamic_cast<const TensorIterator*>(sub)) {
            return ti->get_num_iterations();
        }
        if (const auto l = dynamic_cast<const Loop*>(sub)) {
            return l->get_num_iterations();
        }

        return -1;
    }

    NotValidInputOrOutput not_valid_input_output{-1};
};

}  // namespace detail

Comparator::Result compare_io(ov::op::util::SubGraphOp* sub_lhs, ov::op::util::SubGraphOp* sub_rhs) {
    return detail::CompareSubGraphs{}.compare(sub_lhs, sub_rhs);
}
}  // namespace subgraph
}  // namespace
Comparator::Result Comparator::compare(const std::shared_ptr<ngraph::Function>& f,
                                       const std::shared_ptr<ngraph::Function>& f_ref) {
    /*
     * This function compares two nGraph functions and requires them to have exactly one output
     * + Check nodes types
     * + Check number of inputs
     * + Check shapes
     * + Check parent ports
     * + Check node attributes by Visitor API
     */

    auto f_results = f->get_results();
    auto f_ref_results = f_ref->get_results();

    auto cmp = less_by_name;
    // In case if Result source output has more than one name so the Result may have any of this names as a friendly
    // name An in case of multiple names we sort Result operation using their parent node names
    if (std::any_of(f_results.begin(),
                    f_results.end(),
                    [](const std::shared_ptr<ngraph::Node>& node) {
                        const auto& t = node->input_value(0).get_tensor_ptr();
                        return t->get_names().size() > 1;
                    }) ||
        std::any_of(f_ref_results.begin(), f_ref_results.end(), [](const std::shared_ptr<ngraph::Node>& node) {
            const auto& t = node->input_value(0).get_tensor_ptr();
            return t->get_names().size() > 1;
        })) {
        cmp = less_by_parent_name;
    }

    std::sort(f_results.begin(), f_results.end(), cmp);
    std::sort(f_ref_results.begin(), f_ref_results.end(), cmp);

    if (f_results.size() != f_ref_results.size()) {
        return Result::error("Number of results is different: " + to_str(f_results.size()) + " and " +
                             to_str(f_ref_results.size()));
    }

    const auto& f_sinks = f->get_sinks();
    const auto& f_ref_sinks = f_ref->get_sinks();
    if (f_sinks.size() != f_ref_sinks.size()) {
        return Result::error("Number of sinks is different: " + to_str(f_sinks.size()) + " and " +
                             to_str(f_ref_sinks.size()));
    }

    // Compare sinks
    if (f_sinks.size() == 1) {
        q.push({f_sinks[0].get(), f_ref_sinks[0].get()});
        used.insert(f_sinks[0].get());
    } else {
        // Cast to Assign and find those that have same variable_id suffix
        for (const auto& sink1 : f_sinks) {
            auto assign1 = std::dynamic_pointer_cast<ov::op::util::VariableExtension>(sink1);
            if (!assign1) {
                return Result::error("Sink '" + name(sink1) +
                                     "' is not a variable - graph comparison is not supported");
            }
            auto name1 = assign1->get_variable_id();
            std::shared_ptr<ov::op::Sink> found_sink2;
            for (const auto& sink2 : f_ref_sinks) {
                auto assign2 = std::dynamic_pointer_cast<ov::op::util::VariableExtension>(sink2);
                if (!assign2) {
                    return Result::error("Sink '" + name(sink2) +
                                         "' is not a variable - graph comparison is not supported");
                }
                auto name2 = assign2->get_variable_id();
                if (name2.find(name1) != std::string::npos || name1.find(name2) != std::string::npos) {
                    found_sink2 = sink2;
                    break;
                }
            }
            if (!found_sink2) {
                return Result::error("No suitable sink is found for: " + name(sink1) + ", var=" + name1);
            }
            q.push({sink1.get(), found_sink2.get()});
            used.insert(sink1.get());
        }
    }

    for (size_t i = 0; i < f_results.size(); ++i) {
        if (should_compare(CmpValues::NAMES)) {
            if (name(f_results[i]->get_input_node_shared_ptr(0)) !=
                name(f_ref_results[i]->get_input_node_shared_ptr(0))) {
                return Result::error(
                    "Different output node names: " + name(f_results[i]->get_input_node_shared_ptr(0)) + " and " +
                    name(f_ref_results[i]->get_input_node_shared_ptr(0)));
            }
        }
        q.push({f_results[i].get(), f_ref_results[i].get()});
        used.insert(f_results[i].get());
    }

    std::stringstream errors;

    while (!q.empty()) {
        ngraph::Node* const node1 = q.front().first;
        ngraph::Node* const node2 = q.front().second;
        q.pop();

        const auto result = compare(node1, node2, errors);
        if (!result.valid) {
            return result;
        }

        add_nodes_inputs_to_queue(node1, node2);
    }
    const auto msg = errors.str();
    return msg.empty() ? Result::ok() : Result::error(msg);
}

Comparator::Result Comparator::compare(ngraph::Node* node1, ngraph::Node* node2, std::ostream& err_log) {
    auto type_info1 = node1->get_type_info();
    auto type_info2 = node2->get_type_info();

    if (!compare_type_info(type_info1, type_info2)) {
        return Result::error(typeInfoToStr(type_info1) + " != " + typeInfoToStr(type_info2));
    }

    auto subgraph1 = dynamic_cast<ov::op::util::SubGraphOp*>(node1);
    auto subgraph2 = dynamic_cast<ov::op::util::SubGraphOp*>(node2);

    const bool subgraph_nodes = subgraph1 && subgraph2;

    if (subgraph_nodes) {
        const auto result = subgraph::compare_io(subgraph1, subgraph2);
        if (!result.valid) {
            return result;
        }
    }

    const auto& dependencies_1 = node1->get_control_dependencies();
    const auto& dependencies_2 = node2->get_control_dependencies();

    if (dependencies_1.size() != dependencies_2.size()) {
        return Result::error("Number of dependencies is different: " + to_str(dependencies_1.size()) + " for " +
                             name(node1) + " and " + to_str(dependencies_2.size()) + " for " + name(node2));
    }

    if (node1->inputs().size() != node2->inputs().size()) {
        return Result::error("Number of inputs is different: " + to_str(node1->inputs().size()) + " for " +
                             name(node1) + " and " + to_str(node2->inputs().size()) + " for " + name(node2));
    }

    if (node1->outputs().size() != node2->outputs().size()) {
        return Result::error("Number of outputs is different: " + to_str(node1->inputs().size()) + " for " +
                             name(node1) + " and " + to_str(node2->inputs().size()) + " for " + name(node2));
    }

    if (!subgraph_nodes) {
        compare_inputs(node1, node2, err_log);
        compare_outputs(node1, node2, err_log);
    }

    compare_nodes(node1, node2, err_log);
    return Result::ok("Check if any minor error was log in to err_log");
}

void Comparator::compare_inputs(ngraph::Node* node1, ngraph::Node* node2, std::ostream& err_log) {
    for (size_t i = 0; i < node1->inputs().size(); ++i) {
        if (should_compare(CmpValues::CONST_VALUES)) {
            using Constant = ov::opset8::Constant;
            const auto equal_value = ::attributes::detail::equal::Equal<std::shared_ptr<Constant>>::equal_value;

            auto const1 = ngraph::as_type_ptr<Constant>(node1->get_input_node_shared_ptr(i));
            auto const2 = ngraph::as_type_ptr<Constant>(node2->get_input_node_shared_ptr(i));
            if (const1 && const2 && !equal_value(const1, const2)) {
                err_log << "Different Constant values detected\n"
                        << node1->description() << " Input(" << i << ") and " << node2->description() << " Input(" << i
                        << ")" << std::endl;
            }
        }

        if (should_compare(CmpValues::PRECISIONS)) {
            if (node1->input(i).get_element_type() != node2->input(i).get_element_type()) {
                err_log << "Different element type detected\n"
                        << name(node1) << " Input(" << i << ") " << node1->input(i).get_element_type() << " and "
                        << name(node2) << " Input(" << i << ") " << node2->input(i).get_element_type() << std::endl;
            }
        }

        if (!node1->input(i).get_partial_shape().same_scheme(node2->input(i).get_partial_shape())) {
            err_log << "Different shape detected\n"
                    << name(node1) << " Input(" << i << ") " << node1->input(i).get_partial_shape() << " and "
                    << name(node2) << " Input(" << i << ") " << node2->input(i).get_partial_shape() << std::endl;
        }

        if (node1->get_input_source_output(i).get_index() != node2->get_input_source_output(i).get_index()) {
            auto idx1 = node1->get_input_source_output(i).get_index();
            auto idx2 = node2->get_input_source_output(i).get_index();
            err_log << "Different ports detected\n"
                    << name(node1) << " Input(" << i << ") connected to parent port " << idx1 << " and " << name(node2)
                    << " Input(" << i << ") connected to parent port " << idx2 << std::endl;
        }

        if (should_compare(CmpValues::RUNTIME_KEYS) && !compare_rt_keys(node1->input(i), node2->input(i), err_log)) {
            err_log << "Different runtime info detected at input(" << i << ")\n"
                    << name(node1) << " and " << name(node2) << " not equal runtime info." << std::endl;
        }
    }
}

void Comparator::compare_outputs(ngraph::Node* node1, ngraph::Node* node2, std::ostream& err_log) {
    // Some transformations create new tensors with autogenerated names
    for (int i = 0; i < node1->outputs().size(); ++i) {
        const auto& tensor1 = node1->output(i).get_tensor();
        const auto& tensor2 = node2->output(i).get_tensor();

        if (should_compare(CmpValues::TENSOR_NAMES)) {
            if (tensor1.get_names() != tensor2.get_names()) {
                err_log << "Output tensors names " << tensor_names(tensor1) << " and " << tensor_names(tensor2)
                        << " are different for nodes: " << node1->get_friendly_name() << " and "
                        << node2->get_friendly_name() << std::endl;
            }
        }

        if (!node1->output(i).get_partial_shape().same_scheme(node2->output(i).get_partial_shape())) {
            err_log << "Different shape detected\n"
                    << name(node1) << " Output(" << i << ") " << node1->output(i).get_partial_shape() << " and "
                    << name(node2) << " Output(" << i << ") " << node2->output(i).get_partial_shape() << std::endl;
        }

        if (should_compare(CmpValues::RUNTIME_KEYS) && !compare_rt_keys(node1->output(i), node2->output(i), err_log)) {
            err_log << "Different runtime info detected at output(" << i << ")\n"
                    << name(node1) << " and " << name(node2) << " not equal runtime info." << std::endl;
        }
    }
}

void Comparator::compare_nodes(ngraph::Node* node1, ngraph::Node* node2, std::ostream& err_log) {
    if (should_compare(CmpValues::RUNTIME_KEYS) && !compare_rt_keys(*node1, *node2, err_log)) {
        err_log << "Different runtime info detected\n" + name(node1) + " and " + name(node2) +
                       " not equal runtime info.\n";
    }

    if (should_compare(CmpValues::ATTRIBUTES)) {
        auto res = attributes::compare(node1, node2, m_comparison_flags);
        if (!res.valid)
            err_log << res.message;
    }
}

void Comparator::add_nodes_inputs_to_queue(ngraph::Node* node1, ngraph::Node* node2) {
    for (int i = 0; i < node1->inputs().size(); ++i) {
        if (!used.count(node1->input_value(i).get_node())) {
            q.push({node1->input_value(i).get_node(), node2->input_value(i).get_node()});
            used.insert(node1->input_value(i).get_node());
        }
    }
}

FunctionsComparator::Result FunctionsComparator::compare(const std::shared_ptr<ngraph::Function>& f,
                                                         const std::shared_ptr<ngraph::Function>& f_ref) const {
    return Comparator(m_comparison_flags).compare(f, f_ref);
}

void check_rt_info(const std::shared_ptr<ngraph::Function>& f) {
    static const std::vector<std::string> attrs_to_check{"fused_names_0"};

    std::ostringstream err_log;
    for (auto& op : f->get_ops()) {
        if (ov::op::util::is_constant(op))
            continue;

        const auto& rt_info = op->get_rt_info();
        for (const auto& attr_name : attrs_to_check) {
            if (!rt_info.count(attr_name)) {
                err_log << "Node: " << op->get_friendly_name() << " has no attribute: " << attr_name << std::endl;
            }
        }
    }

    auto err_msg = err_log.str();
    if (!err_msg.empty()) {
        throw ngraph::ngraph_error(err_msg);
    }
}

namespace attributes {
namespace detail {
void ReadAndStoreAttributes::on_adapter(const std::string& name, ngraph::ValueAccessor<void>& adapter) {
    if (auto inputs = ngraph::as_type<ngraph::AttributeAdapter<SubGraphOpInputDescription>>(&adapter)) {
        insert(name, inputs->get());
    } else if (auto outputs = ngraph::as_type<ngraph::AttributeAdapter<SubGraphOpOutputDescription>>(&adapter)) {
        insert(name, outputs->get());
    } else if (ngraph::is_type<ngraph::AttributeAdapter<SpecialBodyPorts>>(&adapter)) {
        // drop comparison, no more info than port indexes which will be check in
        // subgraph::compare_io
    } else if (auto a = ngraph::as_type<ngraph::AttributeAdapter<std::shared_ptr<ngraph::runtime::AlignedBuffer>>>(
                   &adapter)) {
        const auto beg = static_cast<unsigned char*>(a->get()->get_ptr());
        const auto end = beg + a->get()->size();
        insert(name, storage::MemoryChunk{storage::MemoryChunk::Data(beg, end)});
    } else if (auto framework_node_attr =
                   ngraph::as_type<ngraph::AttributeAdapter<ov::op::util::FrameworkNodeAttrs>>(&adapter)) {
        insert(name, framework_node_attr->get());
    } else if (auto variable_ptr =
                   ngraph::as_type<ngraph::AttributeAdapter<std::shared_ptr<ngraph::Variable>>>(&adapter)) {
        insert(name, variable_ptr->get());
    } else if (auto shape_ptr = ngraph::as_type<ngraph::AttributeAdapter<ov::PartialShape>>(&adapter)) {
        insert(name, shape_ptr->get());
    } else if (auto dim_ptr = ngraph::as_type<ngraph::AttributeAdapter<ov::Dimension>>(&adapter)) {
        insert(name, dim_ptr->get());
    } else {
        m_read_result += "store   attr [ ERR ]: " + name + " [drop `void` comparison which is '" +
                         adapter.get_type_info().name + "']";
    }
}
template <typename AttrValue>
void ReadAndCompareAttributes::verify(const std::string& name, const AttrValue& attr_value) {
    if (should_return()) {
        return;
    }
    m_visited_attributes.insert(name);
    const auto ref_value = m_attr_ref.get<AttrValue>(name);
    if (!ref_value) {
        m_cmp_result += "missing attribute name: '" + name + "'";
        return;
    }

    if (!equal::Equal<AttrValue>::equal_value(*ref_value, attr_value)) {
        m_cmp_result += "mismatch in value: '" + name + "' : " + str::Get<AttrValue>::value(*ref_value) + " vs " +
                        str::Get<AttrValue>::value(attr_value);
    }
}

void ReadAndCompareAttributes::verify_mem_buf(const std::string& name,
                                              const std::shared_ptr<ngraph::runtime::AlignedBuffer>& buffer) {
    if (should_return()) {
        return;
    }
    m_visited_attributes.insert(name);
    const auto ref_value = m_attr_ref.get<storage::MemoryChunk>(name);
    if (!ref_value) {
        m_cmp_result += "missing attribute name: '" + name + "'";
        return;
    }

    if (buffer->size() != ref_value->size() ||
        std::memcmp(ref_value->data(), buffer->get_ptr(), ref_value->size()) != 0) {
        m_cmp_result += "mismatch in value: '" + name + "' : look in to the mem buffer";
        return;
    }
}

void ReadAndCompareAttributes::verify_function(const std::string& name, ModelAccessor& adapter) {
    if (should_return()) {
        return;
    }
    m_visited_attributes.insert(name);
    const auto ref_value = m_attr_ref.get<std::shared_ptr<ngraph::Function>>(name);
    if (!ref_value) {
        m_cmp_result += "missing attribute name: '" + name + "'";
        return;
    }
    Comparator c(m_check_flags);
    const auto result = c.compare(*ref_value, adapter.get());
    if (!result.valid) {
        m_cmp_result += result.message;
    }
}

void ReadAndCompareAttributes::verify_others(const std::string& name, ngraph::ValueAccessor<void>& adapter) {
    if (auto inputs = ngraph::as_type<ngraph::AttributeAdapter<SubGraphOpInputDescription>>(&adapter)) {
        verify(name, inputs->get());
    } else if (auto outputs = ngraph::as_type<ngraph::AttributeAdapter<SubGraphOpOutputDescription>>(&adapter)) {
        verify(name, outputs->get());
    } else if (ngraph::is_type<ngraph::AttributeAdapter<SpecialBodyPorts>>(&adapter)) {
        // drop comparison, no more info than port indexes which will be check in
        // subgraph::compare_io
    } else if (auto a = ngraph::as_type<ngraph::AttributeAdapter<std::shared_ptr<ngraph::runtime::AlignedBuffer>>>(
                   &adapter)) {
        verify_mem_buf(name, a->get());
    } else if (auto attrs = ngraph::as_type<ngraph::AttributeAdapter<ov::op::util::FrameworkNodeAttrs>>(&adapter)) {
        verify(name, attrs->get());
    } else if (auto variable_ptr =
                   ngraph::as_type<ngraph::AttributeAdapter<std::shared_ptr<ngraph::Variable>>>(&adapter)) {
        verify(name, variable_ptr->get());
    } else if (auto shape_ptr = ngraph::as_type<ngraph::AttributeAdapter<ov::PartialShape>>(&adapter)) {
        verify(name, shape_ptr->get());
    } else if (auto dim_ptr = ngraph::as_type<ngraph::AttributeAdapter<ov::Dimension>>(&adapter)) {
        verify(name, dim_ptr->get());
    } else {
        m_cmp_result += "compare attr [ ERR ]: " + name + " [drop `void` comparison which is '" +
                        adapter.get_type_info().name + "']";
    }
}

}  // namespace detail

Comparator::Result compare(ngraph::Node* node1, ngraph::Node* node2, Comparator::CmpValues comparition_flags) {
    detail::CompareNodesAttributes compare_nodes_attr(comparition_flags);
    node1->visit_attributes(compare_nodes_attr.get_ref_reader());
    node2->visit_attributes(compare_nodes_attr.get_cmp_reader());
    if (!compare_nodes_attr.equal()) {
        return Comparator::Result::error("Comparison of attributes failed for nodes " + name(node1) + ", " +
                                         name(node2) + " [cmp status: " + to_str(compare_nodes_attr) + "]");
    }
    return Comparator::Result::ok(to_str(compare_nodes_attr));
}

}  // namespace attributes

void accuracy_check(const std::shared_ptr<ov::Model>& ref_function,
                                          const std::shared_ptr<ov::Model>& cur_function) {
    try {
        if (ref_function->is_dynamic() || cur_function->is_dynamic()) {
            return;
        }
        std::map<std::shared_ptr<ov::Node>, ov::Tensor> input_data;
        for (const auto& param : ref_function->get_parameters()) {
            const auto &tensor = ov::test::utils::create_and_fill_tensor(param->get_element_type(),
                                                                         param->get_shape());
            input_data[param] = tensor;
        }

        auto ref_outputs = ngraph::helpers::interpretFunction(ref_function, input_data);
        auto outputs = ngraph::helpers::interpretFunction(ref_function, input_data);

        IE_ASSERT(ref_outputs.size() == outputs.size());

        for (int i=0; i < ref_outputs.size(); i++) {
            ov::test::utils::compare(ref_outputs[i], outputs[i],
                                     std::numeric_limits<double>::max(),
                                     std::numeric_limits<double>::max());
        }
    }
    catch (const std::runtime_error &re) {
        GTEST_FATAL_FAILURE_(re.what());
    } catch (const std::exception &ex) {
        GTEST_FATAL_FAILURE_(ex.what());
    } catch (...) {
        GTEST_FATAL_FAILURE_("Unknown failure occurred.");
    }
}
