# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

set(UNITTEST_CONCURRENT_SRCS
    actor.cj
    api.cj
    backoff.cj
    consistency_model.cj
    consistency_validator.cj
    consistency_validation_result.cj
    concurrent_history.cj
    hbclock.cj
    operation.cj
    partial_order.cj
    permutation_node.cj
    permutation_generator.cj
    scenario.cj
    scenario_builder.cj
    scenario_executor.cj
    scenario_result.cj
    scenario_runner.cj
    sequential_result.cj
    sequential_validator.cj
    topological_permutation_generator.cj
    util.cj
    CACHE INTERNAL "")