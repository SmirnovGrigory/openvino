// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <ngraph/except.hpp>

#include "gtest/gtest.h"

#ifdef SELECTIVE_BUILD_ANALYZER
#    define SELECTIVE_BUILD_ANALYZER_ON
#    undef SELECTIVE_BUILD_ANALYZER
#elif defined(SELECTIVE_BUILD)
#    define SELECTIVE_BUILD_ON
#    undef SELECTIVE_BUILD
#endif

#define SELECTIVE_BUILD_ANALYZER

#include "../core/src/itt.hpp"

using namespace std;

TEST(conditional_compilation, collect_op_scope) {
#define ngraph_op_Scope0 1
    int n = 0;

    // Simple scope is enabled
    NGRAPH_OP_SCOPE(Scope0);
    n = 42;
    EXPECT_EQ(n, 42);

    // Simple scope is disabled
    NGRAPH_OP_SCOPE(Scope1);
    n = 43;
    EXPECT_EQ(n, 43);
#undef CCTests_Scope0
}

#undef SELECTIVE_BUILD_ANALYZER

#ifdef SELECTIVE_BUILD_ANALYZER_ON
#    define SELECTIVE_BUILD_ANALYZER
#elif defined(SELECTIVE_BUILD_ON)
#    define SELECTIVE_BUILD
#endif
