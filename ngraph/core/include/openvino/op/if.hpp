// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include <vector>

#include "openvino/core/function.hpp"
#include "openvino/op/parameter.hpp"
#include "openvino/op/util/multi_subgraph_base.hpp"

namespace ov {
namespace op {
namespace v8 {
/// \brief  If operation.
class OPENVINO_API If : public util::MultiSubGraphOp {
public:
    OPENVINO_OP("If", "opset8", MultiSubGraphOp);
    BWDCMP_RTTI_DECLARATION;

    enum BodyIndexes { THEN_BODY_INDEX = 0, ELSE_BODY_INDEX = 1 };

    bool visit_attributes(AttributeVisitor& visitor) override;

    /// \brief     Constructs If with condition
    ///
    /// \param     execution_condition   condition node.
    If(const Output<Node>& execution_condition);
    If();

    std::shared_ptr<Node> clone_with_new_inputs(const OutputVector& new_args) const override;

    /// \brief     gets then_body as ngraph::Function.
    ///
    /// \return then_body as ngraph::Function.
    const std::shared_ptr<Function>& get_then_body() const {
        return m_bodies[THEN_BODY_INDEX];
    }

    /// \brief     gets else_body as ngraph::Function.
    ///
    /// \return else_body as ngraph::Function.
    const std::shared_ptr<Function>& get_else_body() const {
        return m_bodies[ELSE_BODY_INDEX];
    }

    /// \brief     sets new ngraph::Function as new then_body.
    ///
    /// \param     body   new body for 'then' branch.
    void set_then_body(const std::shared_ptr<Function>& body) {
        m_bodies[THEN_BODY_INDEX] = body;
    }

    /// \brief     sets new ngraph::Function as new else_body.
    ///
    /// \param     body   new body for 'else' branch.
    void set_else_body(const std::shared_ptr<Function>& body) {
        m_bodies[ELSE_BODY_INDEX] = body;
    }

    /// \brief     sets new input to the operation associated with parameters
    /// of each sub-graphs
    ///
    /// \param     value           input to operation
    /// \param     then_parameter  parameter for then_body or nullptr
    /// \param     else_parameter  parameter for else_body or nullpt
    void set_input(const Output<Node>& value,
                   const std::shared_ptr<v0::Parameter>& then_parameter,
                   const std::shared_ptr<v0::Parameter>& else_parameter);

    /// \brief     sets new output from the operation associated with results
    /// of each sub-graphs
    ///
    /// \param     then_result     result from then_body
    /// \param     else_parameter  result from else_body
    /// \return    output from operation
    Output<Node> set_output(const std::shared_ptr<v0::Result>& then_result,
                            const std::shared_ptr<v0::Result>& else_result);

    void validate_and_infer_types() override;
    bool evaluate(const HostTensorVector& outputs, const HostTensorVector& inputs) const override;

    bool has_evaluate() const override;

private:
    using OutputMap = std::map<int64_t, std::shared_ptr<MultiSubGraphOp::OutputDescription>>;

    void validate_and_infer_type_body(const std::shared_ptr<Function>& body,
                                      const MultiSubgraphInputDescriptionVector& input_descriptors);

    OutputMap get_mapping_outputs_on_body_description(const MultiSubgraphOutputDescriptionVector& output_descriptors);
};
}  // namespace v8
}  // namespace op
}  // namespace ov