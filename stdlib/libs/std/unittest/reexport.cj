/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */
package std.unittest

public import std.unittest.common.Configuration
public import std.unittest.common.ConfigurationKey
public import std.unittest.common.DataProvider
public import std.unittest.common.DataStrategy
public import std.unittest.common.DataShrinker
public import std.unittest.common.registerOptionValidator
public import std.unittest.common.setOptionInfo
public import std.unittest.common.OptionValidity
public import std.unittest.common.KeyFor
public import std.unittest.common.KeyTags
public import std.unittest.prop_test.Arbitrary
public import std.unittest.prop_test.Shrink
public import std.unittest.prop_test.random
public import std.unittest.prop_test.KeyRandom

// Re-exports of std.unittest.concurrent public symbols so that
// `import std.unittest.*` (implicitly added by the compiler for test code)
// also brings the concurrent scenario DSL into scope.
public import std.unittest.concurrent.ConcurrentScenario
public import std.unittest.concurrent.ConsistencyModel
public import std.unittest.concurrent.ConsistencyValidationResult
public import std.unittest.concurrent.EquatableObj
public import std.unittest.concurrent.HBClock
public import std.unittest.concurrent.Operation
public import std.unittest.concurrent.OperationInvocation
public import std.unittest.concurrent.ScenarioBuilder
public import std.unittest.concurrent.ScenarioResult
public import std.unittest.concurrent.execResultOf
public import std.unittest.concurrent.operation
public import std.unittest.concurrent.scenario


@When[backend == "cjnative"]
public import std.runtime.blackBox

