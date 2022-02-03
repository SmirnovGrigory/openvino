// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <functional_test_utils/include/functional_test_utils/blob_utils.hpp>
#include "ngraph_test_utils.hpp"
#include <ngraph_functions/utils/ngraph_helpers.hpp>
#include <functional_test_utils/include/functional_test_utils/ov_tensor_utils.hpp>


            using CompareMap = std::map<ov::NodeTypeInfo, std::function<void(
                    const std::shared_ptr<ov::Node> &node,
                    size_t port,
                    const ov::runtime::Tensor &expected,
                    const ov::runtime::Tensor &actual,
                    double absThreshold,
                    double relThreshold)>>;

            //CompareMap getCompareMap();

CompareMap getCompareMap();




namespace {
                void compare(const std::shared_ptr<ov::Node> &node,
                             size_t port,
                             const ov::runtime::Tensor &expected,
                             const ov::runtime::Tensor &actual,
                             double absThreshold,
                             double relThreshold) {
                    ov::test::utils::compare(expected, actual, absThreshold, relThreshold);
                }

                void compare(const std::shared_ptr<ov::op::v0::DetectionOutput> &node,
                             size_t port,
                             const ov::runtime::Tensor &expected,
                             const ov::runtime::Tensor &actual,
                             double absThreshold,
                             double relThreshold) {
                    ASSERT_EQ(expected.get_size(), actual.get_size());

                    size_t expSize = 0;
                    size_t actSize = 0;

                    const float* expBuf = expected.data<const float>();
                    const float* actBuf = actual.data<const float>();
                    ASSERT_NE(expBuf, nullptr);
                    ASSERT_NE(actBuf, nullptr);

                    for (size_t i = 0; i < actual.get_size(); i+=7) {
                        if (expBuf[i] == -1)
                            break;
                        expSize += 7;
                    }
                    for (size_t i = 0; i < actual.get_size(); i+=7) {
                        if (actBuf[i] == -1)
                            break;
                        actSize += 7;
                    }
                    ASSERT_EQ(expSize, actSize);
                    ov::test::utils::compare(expected, actual, 1e-2f, relThreshold);
                }

                template<typename T>
                void compareResults(const std::shared_ptr<ov::Node> &node,
                                    size_t port,
                                    const ov::runtime::Tensor &expected,
                                    const ov::runtime::Tensor &actual,
                                    double absThreshold,
                                    double relThreshold) {
                    return compare(ngraph::as_type_ptr<T>(node), port, expected, actual, absThreshold, relThreshold);
                }

            } // namespace

            CompareMap getCompareMap() {
                CompareMap compareMap{
#define NGRAPH_OP(NAME, NAMESPACE) {NAMESPACE::NAME::get_type_info_static(), compareResults<NAMESPACE::NAME>},

#include "ngraph/opsets/opset1_tbl.hpp"
#include "ngraph/opsets/opset2_tbl.hpp"
#include "ngraph/opsets/opset3_tbl.hpp"
#include "ngraph/opsets/opset4_tbl.hpp"
#include "ngraph/opsets/opset5_tbl.hpp"
#include "ngraph/opsets/opset6_tbl.hpp"
#include "ngraph/opsets/opset7_tbl.hpp"
#include "ngraph/opsets/opset8_tbl.hpp"

#undef NGRAPH_OP
                };
                return compareMap;
            }

namespace {

template<class T_IE, class T_NGRAPH>
static void Compare(const T_NGRAPH *expected, const T_IE *actual, std::size_t size, float threshold, float abs_threshold = -1.f) {
    for (std::size_t i = 0; i < size; ++i) {
        const T_NGRAPH &ref = expected[i];
        const auto &res = actual[i];
        const auto absoluteDifference = CommonTestUtils::ie_abs(res - ref);
        if (abs_threshold > 0.f && absoluteDifference > abs_threshold) {
            IE_THROW() << "Absolute comparison of values expected: " << std::to_string(ref) << " and actual: " << std::to_string(res)
                       << " at index " << i << " with absolute threshold " << abs_threshold
                       << " failed";
        }
        if (absoluteDifference <= threshold) {
            continue;
        }
        double max;
        if (sizeof(T_IE) < sizeof(T_NGRAPH)) {
            max = std::max(CommonTestUtils::ie_abs(T_NGRAPH(res)), CommonTestUtils::ie_abs(ref));
        } else {
            max = std::max(CommonTestUtils::ie_abs(res), CommonTestUtils::ie_abs(T_IE(ref)));
        }
        double diff = static_cast<float>(absoluteDifference) / max;
        if (max == 0 || (diff > static_cast<float>(threshold)) ||
            (std::isnan(static_cast<float>(res)) ^ std::isnan(static_cast<float>(ref)))) {
            IE_THROW() << "Relative comparison of values expected: " << std::to_string(ref) << " and actual: " << std::to_string(res)
                       << " at index " << i << " with threshold " << threshold
                       << " failed";
        }
    }
}

template <typename T_IE>
void callCompare(const std::pair<ngraph::element::Type, std::vector<std::uint8_t>> &expected,
                 const T_IE* actualBuffer, size_t size, float threshold, float abs_threshold) {
    auto expectedBuffer = expected.second.data();
    switch (expected.first) {
        case ngraph::element::Type_t::i64:
            Compare<T_IE, int64_t>(reinterpret_cast<const int64_t *>(expectedBuffer),
                                                         actualBuffer, size, threshold, abs_threshold);
            break;
        case ngraph::element::Type_t::i32:
            Compare<T_IE, int32_t>(reinterpret_cast<const int32_t *>(expectedBuffer),
                                                         actualBuffer, size, threshold, abs_threshold);
            break;
        case ngraph::element::Type_t::i16:
            Compare<T_IE, int16_t>(reinterpret_cast<const int16_t *>(expectedBuffer),
                                                         actualBuffer, size, threshold, abs_threshold);
            break;
        case ngraph::element::Type_t::i8:
            Compare<T_IE, int8_t>(reinterpret_cast<const int8_t *>(expectedBuffer),
                                                        actualBuffer, size, threshold, abs_threshold);
            break;
        case ngraph::element::Type_t::u64:
            Compare<T_IE, uint64_t>(reinterpret_cast<const uint64_t *>(expectedBuffer),
                                                          actualBuffer, size, threshold, abs_threshold);
            break;
        case ngraph::element::Type_t::u32:
            Compare<T_IE, uint32_t>(reinterpret_cast<const uint32_t *>(expectedBuffer),
                                                          actualBuffer, size, threshold, abs_threshold);
            break;
        case ngraph::element::Type_t::u16:
            Compare<T_IE, uint16_t>(reinterpret_cast<const uint16_t *>(expectedBuffer),
                                                          actualBuffer, size, threshold, abs_threshold);
            break;
        case ngraph::element::Type_t::boolean:
        case ngraph::element::Type_t::u8:
            Compare<T_IE, uint8_t>(reinterpret_cast<const uint8_t *>(expectedBuffer),
                                                         actualBuffer, size, threshold, abs_threshold);
            break;
        case ngraph::element::Type_t::f64:
            Compare<T_IE, double>(reinterpret_cast<const double *>(expectedBuffer),
                                                        actualBuffer, size, threshold, abs_threshold);
            break;
        case ngraph::element::Type_t::f32:
            Compare<T_IE, float>(reinterpret_cast<const float *>(expectedBuffer),
                                                       actualBuffer, size, threshold, abs_threshold);
            break;
        case ngraph::element::Type_t::f16:
            Compare<T_IE, ngraph::float16>(reinterpret_cast<const ngraph::float16 *>(expectedBuffer),
                                                                 actualBuffer, size, threshold, abs_threshold);
            break;
        case ngraph::element::Type_t::bf16:
            Compare<T_IE, ngraph::bfloat16>(reinterpret_cast<const ngraph::bfloat16 *>(expectedBuffer),
                                                                  actualBuffer, size, threshold, abs_threshold);
            break;
//        case ngraph::element::Type_t::i4: {
//            auto expectedOut = ngraph::helpers::convertOutputPrecision(
//                    expected.second,
//                    expected.first,
//                    ngraph::element::Type_t::i8,
//                    size);
//            Compare<T_IE, int8_t>(reinterpret_cast<const int8_t *>(expectedOut.data()),
//                                                        actualBuffer, size, threshold, abs_threshold);
//            break;
//        }
//        case ngraph::element::Type_t::u4: {
//            auto expectedOut = ngraph::helpers::convertOutputPrecision(
//                    expected.second,
//                    expected.first,
//                    ngraph::element::Type_t::u8,
//                    size);
//            Compare<T_IE, uint8_t>(reinterpret_cast<const uint8_t *>(expectedOut.data()),
//                                                         actualBuffer, size, threshold, abs_threshold);
//            break;
//        }
        case ngraph::element::Type_t::dynamic:
        case ngraph::element::Type_t::undefined:
            Compare<T_IE, T_IE>(reinterpret_cast<const T_IE *>(expectedBuffer), actualBuffer, size, threshold, abs_threshold);
            break;
        default: FAIL() << "Comparator for " << expected.first << " precision isn't supported";
    }
    return;
}


void Compare(const std::pair<ngraph::element::Type, std::vector<std::uint8_t>> &expected,
                    const InferenceEngine::Blob::Ptr &actual,
                    float threshold,
                    float abs_threshold) {
    const auto &precision = actual->getTensorDesc().getPrecision();
    auto k =  static_cast<float>(expected.first.size()) / precision.size();
    // W/A for int4, uint4
    if (expected.first == ngraph::element::Type_t::u4 || expected.first == ngraph::element::Type_t::i4) {
        k /= 2;
    } else if (expected.first == ngraph::element::Type_t::undefined || expected.first == ngraph::element::Type_t::dynamic) {
        k = 1;
    }
    ASSERT_EQ(expected.second.size(), actual->byteSize() * k);

    auto memory = InferenceEngine::as<InferenceEngine::MemoryBlob>(actual);
    IE_ASSERT(memory);
    const auto lockedMemory = memory->wmap();
    const auto actualBuffer = lockedMemory.as<const std::uint8_t *>();

    const auto &size = actual->size();
    switch (precision) {
        case InferenceEngine::Precision::FP32:
            callCompare<float>(expected, reinterpret_cast<const float *>(actualBuffer), size, threshold, abs_threshold);
            break;
        case InferenceEngine::Precision::I32:
            callCompare<int32_t>(expected, reinterpret_cast<const int32_t *>(actualBuffer), size, threshold, abs_threshold);
            break;
        case InferenceEngine::Precision::I64:
            callCompare<int64_t>(expected, reinterpret_cast<const int64_t *>(actualBuffer), size, threshold, abs_threshold);
            break;
        case InferenceEngine::Precision::I8:
            callCompare<int8_t>(expected, reinterpret_cast<const int8_t *>(actualBuffer), size, threshold, abs_threshold);
            break;
        case InferenceEngine::Precision::U16:
            callCompare<uint16_t>(expected, reinterpret_cast<const uint16_t *>(actualBuffer), size, threshold, abs_threshold);
            break;
        case InferenceEngine::Precision::I16:
            callCompare<int16_t>(expected, reinterpret_cast<const int16_t *>(actualBuffer), size, threshold, abs_threshold);
            break;
        case InferenceEngine::Precision::BOOL:
        case InferenceEngine::Precision::U8:
            callCompare<uint8_t>(expected, reinterpret_cast<const uint8_t *>(actualBuffer), size, threshold, abs_threshold);
            break;
        case InferenceEngine::Precision::U64:
            callCompare<uint64_t>(expected, reinterpret_cast<const uint64_t *>(actualBuffer), size, threshold, abs_threshold);
            break;
        case InferenceEngine::Precision::BF16:
            callCompare<ngraph::bfloat16>(expected, reinterpret_cast<const ngraph::bfloat16 *>(actualBuffer), size, threshold, abs_threshold);
            break;
        case InferenceEngine::Precision::FP16:
            callCompare<ngraph::float16>(expected, reinterpret_cast<const ngraph::float16 *>(actualBuffer), size, threshold, abs_threshold);
            break;
        default:
            FAIL() << "Comparator for " << precision << " precision isn't supported";
    }
}

void Compare(const std::vector<std::pair<ngraph::element::Type, std::vector<std::uint8_t>>> &expectedOutputs,
                    const std::vector<InferenceEngine::Blob::Ptr> &actualOutputs,
                    float threshold, float abs_threshold) {
    for (std::size_t outputIndex = 0; outputIndex < expectedOutputs.size(); ++outputIndex) {
        const auto &expected = expectedOutputs[outputIndex];
        const auto &actual = actualOutputs[outputIndex];
        Compare(expected, actual, threshold, abs_threshold);
    }
}
} // namespace

void compare(const std::shared_ptr<ov::Model> &function,
             const std::vector<ov::Tensor>& expected,
             const std::vector<ov::Tensor>& actual) {
    ASSERT_EQ(expected.size(), actual.size());
    ASSERT_EQ(expected.size(), function->get_results().size());
    auto compareMap = getCompareMap();
    const auto& results = function->get_results();
    for (size_t j = 0; j < results.size(); j++) {
        const auto result = results[j];
        for (size_t i = 0; i < result->get_input_size(); ++i) {
            std::shared_ptr<ov::Node> inputNode = result->get_input_node_shared_ptr(i);
//            if (std::dynamic_pointer_cast<ov::op::v0::Convert>(inputNode)) {
//                std::shared_ptr<ov::Node> nextNodePtr = inputNode->get_input_node_shared_ptr(0);
//                if (!ngraph::is_type<ov::op::v0::Result>(nextNodePtr)) {
//                    inputNode = nextNodePtr;
//                }
//            }
            auto it = compareMap.find(inputNode->get_type_info());
            it->second(inputNode, i, expected[j], actual[j],
                       std::numeric_limits<double>::max(),
                       std::numeric_limits<double>::max());
        }
    }
}

void TransformationTestsF::accuracy_check(const std::shared_ptr<ov::Model>& ref_function,
                                          const std::shared_ptr<ov::Model>& cur_function) {
    try {
        if (ref_function->is_dynamic() || cur_function->is_dynamic()) {
            return;
        }
        //std::vector<std::vector<uint8_t>> input_data;
        std::map<std::shared_ptr<ov::Node>, ov::Tensor> input_data;
        ngraph::element::TypeVector types;
        for (const auto& param : ref_function->get_parameters()) {
            types.push_back(param->get_element_type());

            auto layout = InferenceEngine::Layout::ANY;
            if (ov::is_scalar(param->get_shape())) {
                layout = InferenceEngine::Layout::SCALAR;
            }
            const auto &tensor = ov::test::utils::create_and_fill_tensor(param->get_element_type(),
                                                                        param->get_shape());
//            InferenceEngine::TensorDesc td(InferenceEngine::Precision::FP32, param->get_shape(), layout);
//            const auto &input = FuncTestUtils::createAndFillBlob(td);
//            const auto &input_size = input->byteSize();
//
//            std::vector<uint8_t> data;
//            data.resize(input_size);
//
//            auto memory = InferenceEngine::as<InferenceEngine::MemoryBlob>(input);
//            IE_ASSERT(memory);
//
//            const auto lockedMemory = memory->wmap();
//            const auto buffer = lockedMemory.as<const std::uint8_t *>();
//            std::copy(buffer, buffer + input_size, data.data());
//
//            input_data.push_back(std::move(data));
            input_data[param] = tensor;
        }

//        auto ref_outputs = ngraph::helpers::interpreterFunction(ref_function, input_data, types);
//        auto outputs = ngraph::helpers::interpreterFunction(cur_function, input_data, types);

        auto ref_outputs = ngraph::helpers::interpretFunction(ref_function, input_data);
        auto outputs = ngraph::helpers::interpretFunction(ref_function, input_data);

        IE_ASSERT(ref_outputs.size() == outputs.size());


//        for (size_t i = 0; i < ref_outputs.size(); ++i) {
//            IE_ASSERT(ref_outputs[i].second.size() == outputs[i].second.size());
//            auto * ref = reinterpret_cast<float *>(ref_outputs[i].second.data());
//            auto * out = reinterpret_cast<float *>(outputs[i].second.data());
//            size_t size = ref_outputs[i].second.size() / sizeof(float);
//            IE_ASSERT(size > 0);
//            Compare<float, float>(ref, out, size, 1e-5);
//        }
    }
    catch (const std::runtime_error &re) {
        GTEST_FATAL_FAILURE_(re.what());
    } catch (const std::exception &ex) {
        GTEST_FATAL_FAILURE_(ex.what());
    } catch (...) {
        GTEST_FATAL_FAILURE_("Unknown failure occurred.");
    }
}

void init_unique_names(std::shared_ptr<ngraph::Function> f, const std::shared_ptr<ngraph::pass::UniqueNamesHolder>& unh) {
    ngraph::pass::Manager manager;
    manager.register_pass<ngraph::pass::InitUniqueNames>(unh);
    manager.run_passes(f);
}

void check_unique_names(std::shared_ptr<ngraph::Function> f, const std::shared_ptr<ngraph::pass::UniqueNamesHolder>& unh) {
    ngraph::pass::Manager manager;
    manager.register_pass<ngraph::pass::CheckUniqueNames>(unh, true);
    manager.run_passes(f);
}
