// tests/unit/test_result.cpp — the Result/Error primitives
//
// Result is the vocabulary every other layer speaks, so what is worth pinning
// here is the contract its callers rely on: which code each factory sets, the
// exact strings toString puts on the wire, that a failed Result carries no
// usable value, and that the documented programming-error cases still throw
// rather than being papered over with a default.

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "core/Result.hpp"

namespace
{

// A stand-in for the real call pattern: a function returning Result<T> that
// signals failure by returning an Error. It is declared out here so the
// implicit conversion is exercised where it actually matters — a return
// statement at a layer boundary, not a hand-built temporary in a test.
taskpilot::Result<std::string> lookup(std::int64_t id)
{
    if (7 == id)
    {
        return std::string{ "task seven" };
    }
    return taskpilot::Error::notFound("no task with that id");
}

// The Status alias gets the same treatment, since void-ish write operations
// are the majority of the store's error paths.
taskpilot::Status writeRows(int rows)
{
    if (0 == rows)
    {
        return taskpilot::Error::conflict("nothing to write");
    }
    return taskpilot::Unit{};
}

} // namespace

TEST(ResultTest, ErrorFactoriesSetTheirOwnCode)
{
    EXPECT_EQ(taskpilot::ErrorCode::kInvalidArgument,
              taskpilot::Error::invalidArgument("x").code);
    EXPECT_EQ(taskpilot::ErrorCode::kNotFound, taskpilot::Error::notFound("x").code);
    EXPECT_EQ(taskpilot::ErrorCode::kConflict, taskpilot::Error::conflict("x").code);
    EXPECT_EQ(taskpilot::ErrorCode::kStorageFailure,
              taskpilot::Error::storageFailure("x").code);
    EXPECT_EQ(taskpilot::ErrorCode::kInternal, taskpilot::Error::internal("x").code);
}

TEST(ResultTest, ErrorFactoriesPreserveTheMessage)
{
    const taskpilot::Error error = taskpilot::Error::notFound("task 7 does not exist");
    EXPECT_EQ(std::string("task 7 does not exist"), error.message);
}

TEST(ResultTest, ErrorCodeNamesAreStable)
{
    // These strings are an API, not a debug convenience: the control layer
    // copies one into the `data` field of a JSON-RPC error and clients branch
    // on it. Asserting the exact spelling is the only way a rename gets
    // caught before it reaches a client.
    EXPECT_EQ(std::string("invalid_argument"),
              taskpilot::toString(taskpilot::ErrorCode::kInvalidArgument));
    EXPECT_EQ(std::string("not_found"), taskpilot::toString(taskpilot::ErrorCode::kNotFound));
    EXPECT_EQ(std::string("conflict"), taskpilot::toString(taskpilot::ErrorCode::kConflict));
    EXPECT_EQ(std::string("storage_failure"),
              taskpilot::toString(taskpilot::ErrorCode::kStorageFailure));
    EXPECT_EQ(std::string("internal"), taskpilot::toString(taskpilot::ErrorCode::kInternal));
}

TEST(ResultTest, DefaultConstructedErrorIsInternal)
{
    // The member initializer is a safety net for logging code that declares
    // an Error{} and fills in only some fields. It must not default to a
    // caller-blaming code like kInvalidArgument.
    const taskpilot::Error error{};
    EXPECT_EQ(taskpilot::ErrorCode::kInternal, error.code);
    EXPECT_TRUE(error.message.empty());
}

TEST(ResultTest, OkResultExposesItsValue)
{
    const taskpilot::Result<int> result{ 42 };
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(42, result.value());
}

TEST(ResultTest, FailedResultExposesItsError)
{
    const taskpilot::Result<int> result = taskpilot::Error::storageFailure("disk is full");
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(taskpilot::ErrorCode::kStorageFailure, result.error().code);
    EXPECT_EQ(std::string("disk is full"), result.error().message);
}

TEST(ResultTest, ImplicitConversionFromErrorOnReturn)
{
    const taskpilot::Result<std::string> found = lookup(7);
    ASSERT_TRUE(found.ok());
    EXPECT_EQ(std::string("task seven"), found.value());

    // The failure branch: `return Error::notFound(...)` converted implicitly,
    // so a call site writes one line instead of wrapping the error.
    const taskpilot::Result<std::string> missing = lookup(8);
    ASSERT_FALSE(missing.ok());
    EXPECT_EQ(taskpilot::ErrorCode::kNotFound, missing.error().code);
}

TEST(ResultTest, StatusCarriesNoPayload)
{
    const taskpilot::Status written = writeRows(3);
    EXPECT_TRUE(written.ok());

    const taskpilot::Status refused = writeRows(0);
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(taskpilot::ErrorCode::kConflict, refused.error().code);
}

TEST(ResultTest, MutableValueIsWritable)
{
    taskpilot::Result<std::vector<int>> result{ std::vector<int>{ 1, 2 } };
    result.value().push_back(3);
    EXPECT_EQ(std::size_t{ 3 }, result.value().size());
    EXPECT_EQ(3, result.value().back());
}

TEST(ResultTest, ValueCanBeMovedOutOfAnRvalueResult)
{
    // The rvalue overload exists so a caller can unwrap a Result returned by
    // value without copying its payload. A move-only T is the only way to
    // prove the compiler picked that overload: the const-ref one would be a
    // compile error here, since it cannot copy a unique_ptr.
    taskpilot::Result<std::unique_ptr<int>> result{ std::make_unique<int>(5) };

    const std::unique_ptr<int> moved = std::move(result).value();
    ASSERT_TRUE(moved != nullptr);
    EXPECT_EQ(5, *moved);
}

TEST(ResultTest, ValueOrPrefersTheValue)
{
    const taskpilot::Result<int> result{ 42 };
    EXPECT_EQ(42, result.valueOr(9));

    // valueOr does not consume or mutate the Result, so the caller can still
    // unwrap it afterwards.
    EXPECT_EQ(42, result.value());
}

TEST(ResultTest, ValueOrFallsBackOnFailure)
{
    const taskpilot::Result<int> result = taskpilot::Error::internal("boom");
    EXPECT_EQ(9, result.valueOr(9));
}

TEST(ResultTest, ResultsCopyLikeOrdinaryValues)
{
    // The header promises a Result is an ordinary copyable value; several
    // call sites rely on that when passing one on to a caller.
    const taskpilot::Result<std::string> original{ std::string("hello") };
    const taskpilot::Result<std::string> copy = original;
    EXPECT_EQ(std::string("hello"), copy.value());

    const taskpilot::Result<std::string> failure = taskpilot::Error::conflict("stale version");
    const taskpilot::Result<std::string> failure_copy = failure;
    EXPECT_EQ(taskpilot::ErrorCode::kConflict, failure_copy.error().code);
}

TEST(ResultTest, ValueOnFailedResultThrows)
{
    // Documented precondition: callers check ok() first. The throw is the
    // backstop that turns a forgotten check into a crash in a test rather
    // than a plausible-looking default value in production.
    const taskpilot::Result<std::string> result = taskpilot::Error::internal("boom");
    EXPECT_THROW({ (void)result.value(); }, std::bad_variant_access);
}

TEST(ResultTest, ErrorOnOkResultThrows)
{
    const taskpilot::Result<int> result{ 1 };
    EXPECT_THROW({ (void)result.error(); }, std::bad_variant_access);
}
