// test_weights.cpp — validation and JSON round-trip of the ranking coefficients.
//
// These tests pin the two failures that are invisible from the outside:
//   1. A weight that is silently ignored — the tuning appears to have no effect
//      and the caller cannot tell a rejected request from an ineffective one.
//   2. A weight that is silently accepted but poisons the score — a NaN makes
//      every comparison false, a negative inverts a term, and in both cases the
//      only symptom is a queue that looks wrong.
// Both are cheap to catch here and expensive to diagnose from a queue.

#include <cmath>
#include <limits>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/Result.hpp"
#include "core/Weights.hpp"

namespace taskpilot
{

namespace
{

/// The four score weights, in the order Weights.hpp documents them. The
/// boundary tests loop over all four on purpose: a future weight must not be
/// able to slip in with a weaker check than its neighbours.
constexpr const char *kWeightKeys[] = { "importance", "urgency", "age", "blocks" };

/// A body with every field present and valid, used as the base for the "one
/// field is broken" cases so a failure can only come from the field under test.
nlohmann::json validBody()
{
    return toJson(Weights{});
}

} // namespace

TEST(WeightsTest, DefaultsAreValid)
{
    const Weights weights;

    EXPECT_TRUE(weights.validate().ok());

    // The defaults are part of the contract: they are what a fresh database is
    // seeded with, and docs/scoring.md's worked example scores against them.
    EXPECT_DOUBLE_EQ(5.0, weights.importance);
    EXPECT_DOUBLE_EQ(30.0, weights.urgency);
    EXPECT_DOUBLE_EQ(2.0, weights.age);
    EXPECT_DOUBLE_EQ(8.0, weights.blocks);
    EXPECT_DOUBLE_EQ(7.0, weights.urgency_horizon_days);
}

TEST(WeightsTest, ToJsonEmitsAllFiveFields)
{
    const nlohmann::json encoded = toJson(Weights{});

    // A stable, complete schema. A client reading a weights object should not
    // have to know which keys this build chose to omit.
    EXPECT_EQ(5u, encoded.size());
    EXPECT_TRUE(encoded.contains("importance"));
    EXPECT_TRUE(encoded.contains("urgency"));
    EXPECT_TRUE(encoded.contains("age"));
    EXPECT_TRUE(encoded.contains("blocks"));
    EXPECT_TRUE(encoded.contains("urgency_horizon_days"));

    EXPECT_DOUBLE_EQ(5.0, encoded.at("importance").get<double>());
    EXPECT_DOUBLE_EQ(30.0, encoded.at("urgency").get<double>());
    EXPECT_DOUBLE_EQ(2.0, encoded.at("age").get<double>());
    EXPECT_DOUBLE_EQ(8.0, encoded.at("blocks").get<double>());
    EXPECT_DOUBLE_EQ(7.0, encoded.at("urgency_horizon_days").get<double>());
}

TEST(WeightsTest, ZeroIsAllowedForEveryScoreWeight)
{
    // Zeroing a weight is the supported way to switch a term off
    // (docs/scoring.md), so 0.0 is a legal boundary and must not be mistaken
    // for a missing or invalid value.
    for (const char *key : kWeightKeys)
    {
        nlohmann::json body = validBody();
        body[key] = 0.0;

        const Result<Weights> parsed = weightsFromJson(body);
        EXPECT_TRUE(parsed.ok()) << "key: " << key;
    }
}

TEST(WeightsTest, AllZeroWeightsAreValid)
{
    // Zeroing all four is the documented "pure FIFO" mode: the queue then falls
    // back entirely to the tie-breakers (oldest first). It is a defensible
    // configuration, not an error.
    Weights weights;
    weights.importance = 0.0;
    weights.urgency = 0.0;
    weights.age = 0.0;
    weights.blocks = 0.0;

    EXPECT_TRUE(weights.validate().ok());
}

TEST(WeightsTest, ZeroHorizonIsRejected)
{
    // The horizon is the denominator of the urgency ramp, so the boundary that
    // is legal for a weight is a division by zero here.
    nlohmann::json body = validBody();
    body["urgency_horizon_days"] = 0.0;

    const Result<Weights> parsed = weightsFromJson(body);

    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(ErrorCode::kInvalidArgument, parsed.error().code);
    EXPECT_NE(std::string::npos, parsed.error().message.find("urgency_horizon_days"));
}

TEST(WeightsTest, NegativeHorizonIsRejected)
{
    // A negative horizon would make urgency grow as the deadline recedes, which
    // is the opposite of what the term means.
    Weights weights;
    weights.urgency_horizon_days = -1.0;

    const Status status = weights.validate();

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(ErrorCode::kInvalidArgument, status.error().code);
    EXPECT_NE(std::string::npos, status.error().message.find("urgency_horizon_days"));
}

TEST(WeightsTest, NegativeWeightNamesTheFieldAndValue)
{
    // The exact message matters when five numbers travel in one request:
    // "invalid weights" would leave the caller to bisect its own patch.
    Weights weights;
    weights.urgency = -1.0;

    const Status status = weights.validate();

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(std::string{ "weights.urgency must be >= 0 (got -1)" }, status.error().message);
}

TEST(WeightsTest, EveryNegativeWeightIsRejectedAndReported)
{
    for (const char *key : kWeightKeys)
    {
        nlohmann::json body = validBody();
        body[key] = -0.5;

        const Result<Weights> parsed = weightsFromJson(body);

        ASSERT_FALSE(parsed.ok()) << "key: " << key;
        EXPECT_EQ(ErrorCode::kInvalidArgument, parsed.error().code);
        EXPECT_NE(std::string::npos, parsed.error().message.find(key));
        EXPECT_NE(std::string::npos, parsed.error().message.find("-0.5"));
    }
}

TEST(WeightsTest, WeightAboveTheUpperBoundIsRejectedAndReported)
{
    // A finite weight is not automatically a safe one: what reaches the wire
    // is the weight's PRODUCT with a task field, so a weight big enough to
    // overflow that product has to be refused even though the double itself is
    // perfectly legal. The message names the field and the value, like every
    // other rule violation in this file.
    //
    // 1e10 is a full order above the bound, so this failure cannot be confused
    // with rounding at the boundary — that case has its own test below.
    for (const char *key : kWeightKeys)
    {
        nlohmann::json body = validBody();
        body[key] = 1e10;

        const Result<Weights> parsed = weightsFromJson(body);

        ASSERT_FALSE(parsed.ok()) << "key: " << key;
        EXPECT_EQ(ErrorCode::kInvalidArgument, parsed.error().code);
        EXPECT_NE(std::string::npos, parsed.error().message.find(key));
        EXPECT_NE(std::string::npos, parsed.error().message.find("1e+10"));
    }
}

TEST(WeightsTest, WeightExactlyAtTheUpperBoundIsAccepted)
{
    // The ceiling is inclusive: a weight of exactly kMaxWeight still multiplies
    // into a finite product, so it must be legal. Pinning the constant here
    // keeps the boundary tests meaningful — they are written against a
    // documented 1e9, not against whatever the constant happens to hold.
    EXPECT_DOUBLE_EQ(1e9, kMaxWeight);

    Weights weights;
    weights.importance = kMaxWeight;
    weights.urgency = kMaxWeight;
    weights.age = kMaxWeight;
    weights.blocks = kMaxWeight;

    EXPECT_TRUE(weights.validate().ok());

    // One representable step above the bound is refused, which pins the other
    // side of the boundary: the rule is "<= kMaxWeight", not "<".
    weights.blocks = std::nextafter(kMaxWeight, std::numeric_limits<double>::infinity());

    const Status status = weights.validate();

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(ErrorCode::kInvalidArgument, status.error().code);
    EXPECT_NE(std::string::npos, status.error().message.find("blocks"));
}

TEST(WeightsTest, WeightWhoseProductWouldOverflowIsRejected)
{
    // The regression the bound exists for: 1e308 is finite, so the finiteness
    // rule accepted it, yet 1e308 * 5 overflows to +inf and the score reached
    // the wire as JSON null. A field that clients parse as a number silently
    // stopped being one, and nothing on the path raised an error.
    EXPECT_FALSE(std::isfinite(1e308 * 5.0));

    nlohmann::json body = validBody();
    body["importance"] = 1e308;

    const Result<Weights> parsed = weightsFromJson(body);

    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(ErrorCode::kInvalidArgument, parsed.error().code);
    EXPECT_NE(std::string::npos, parsed.error().message.find("importance"));
    EXPECT_NE(std::string::npos, parsed.error().message.find("1e+308"));

    // The same value on a hand-built struct: validate() is the single gate the
    // parsed and the constructed path share, so the two must not disagree
    // about what is legal.
    Weights weights;
    weights.urgency = 1e308;

    EXPECT_FALSE(weights.validate().ok());
}

TEST(WeightsTest, NonFiniteWeightsAreRejected)
{
    // NaN is the worst case: it compares false against everything, so it would
    // slip through a naive "value < 0" range check and leave the queue
    // unordered. Infinity would pin one task to the top of the queue forever.
    const double kBadValues[] = {
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
    };

    for (const char *key : kWeightKeys)
    {
        for (const double bad : kBadValues)
        {
            nlohmann::json body = validBody();
            body[key] = bad;

            const Result<Weights> parsed = weightsFromJson(body);

            ASSERT_FALSE(parsed.ok()) << "key: " << key;
            EXPECT_EQ(ErrorCode::kInvalidArgument, parsed.error().code);
            EXPECT_NE(std::string::npos, parsed.error().message.find(key));
            EXPECT_NE(std::string::npos, parsed.error().message.find("finite"));
        }
    }
}

TEST(WeightsTest, NonFiniteHorizonIsRejected)
{
    // The horizon is not a weight but it is parsed out of the same object, so it
    // gets the same finiteness guard: an infinite horizon means the urgency ramp
    // becomes 1 - finite/infinite, a value that never reaches its intended range.
    const double kBadValues[] = {
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),
    };

    for (const double bad : kBadValues)
    {
        nlohmann::json body = validBody();
        body["urgency_horizon_days"] = bad;

        const Result<Weights> parsed = weightsFromJson(body);

        ASSERT_FALSE(parsed.ok());
        EXPECT_EQ(ErrorCode::kInvalidArgument, parsed.error().code);
        EXPECT_NE(std::string::npos, parsed.error().message.find("urgency_horizon_days"));
        EXPECT_NE(std::string::npos, parsed.error().message.find("finite"));
    }
}

TEST(WeightsTest, NonFiniteWeightCannotSurviveARoundTrip)
{
    // Belt and braces with validate(): JSON has no NaN literal, so nlohmann
    // writes a non-finite double as null and the value cannot even be recorded.
    // The point of the test is that neither direction can smuggle it through.
    Weights weights;
    weights.importance = std::numeric_limits<double>::quiet_NaN();

    // Rejection in memory, where the double is still a NaN.
    const nlohmann::json encoded = toJson(weights);
    const Result<Weights> inMemory = weightsFromJson(encoded);
    ASSERT_FALSE(inMemory.ok());
    EXPECT_EQ(ErrorCode::kInvalidArgument, inMemory.error().code);
    EXPECT_NE(std::string::npos, inMemory.error().message.find("importance"));

    // Rejection after a real persist-and-reload trip, where it is null.
    const nlohmann::json reparsed = nlohmann::json::parse(encoded.dump());
    const Result<Weights> throughWire = weightsFromJson(reparsed);
    ASSERT_FALSE(throughWire.ok());
    EXPECT_EQ(ErrorCode::kInvalidArgument, throughWire.error().code);
    EXPECT_NE(std::string::npos, throughWire.error().message.find("importance"));
}

TEST(WeightsTest, UnknownKeysAreIgnored)
{
    // A newer client sends a key this build has never heard of — a sixth weight,
    // or a whole extra object. Ignoring it is what keeps the two compatible; the
    // keys this build does know must still take effect.
    nlohmann::json body = validBody();
    body["urgency"] = 12.5;
    body["future_weight"] = 3.0;
    body["future_config"] = nlohmann::json{ { "urgency", 999.0 } };
    body["also_unknown"] = nlohmann::json::array({ 1.0, 2.0, 3.0 });

    const Result<Weights> parsed = weightsFromJson(body);

    ASSERT_TRUE(parsed.ok());
    EXPECT_DOUBLE_EQ(12.5, parsed.value().urgency);
    EXPECT_DOUBLE_EQ(5.0, parsed.value().importance);
}

TEST(WeightsTest, WrongTypeIsRejectedAndNamesTheField)
{
    // A present-but-wrong-typed value is an error, never a quiet fallback to the
    // default: a weight change silently ignored is indistinguishable from one
    // that failed, and the caller would tune the queue for an hour wondering why
    // nothing moved.
    const nlohmann::json kBadValues[] = {
        nlohmann::json(nullptr),
        nlohmann::json("30"),
        nlohmann::json(true),
        nlohmann::json::array({ 30.0 }),
        nlohmann::json::object({ { "value", 30.0 } }),
    };

    for (const char *key : kWeightKeys)
    {
        for (const nlohmann::json &bad : kBadValues)
        {
            nlohmann::json body = validBody();
            body[key] = bad;

            const Result<Weights> parsed = weightsFromJson(body);

            ASSERT_FALSE(parsed.ok()) << "key: " << key;
            EXPECT_EQ(ErrorCode::kInvalidArgument, parsed.error().code);
            EXPECT_NE(std::string::npos, parsed.error().message.find(key));
        }
    }
}

TEST(WeightsTest, WrongTypeForHorizonIsRejected)
{
    nlohmann::json body = validBody();
    body["urgency_horizon_days"] = "7";

    const Result<Weights> parsed = weightsFromJson(body);

    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(ErrorCode::kInvalidArgument, parsed.error().code);
    EXPECT_NE(std::string::npos, parsed.error().message.find("urgency_horizon_days"));
}

TEST(WeightsTest, NonObjectBodyIsRejected)
{
    // There are no field names inside a bare number or a null, so there is
    // nothing to validate and nothing to default to: the request is malformed.
    const nlohmann::json kBadBodies[] = {
        nlohmann::json(nullptr),
        nlohmann::json(7.0),
        nlohmann::json("weights"),
        nlohmann::json(true),
        nlohmann::json::array({ 1.0, 2.0 }),
    };

    for (const nlohmann::json &body : kBadBodies)
    {
        const Result<Weights> parsed = weightsFromJson(body);

        ASSERT_FALSE(parsed.ok());
        EXPECT_EQ(ErrorCode::kInvalidArgument, parsed.error().code);
    }
}

TEST(WeightsTest, IntegerJsonIsAccepted)
{
    // JSON's integer/float distinction is a spelling detail, not a different
    // value: "importance": 5 and "importance": 5.0 mean the same thing, and a
    // client that emits the former must not be told its request was malformed.
    nlohmann::json body = validBody();
    body["importance"] = 5;
    body["blocks"] = 0;
    body["urgency_horizon_days"] = 14;

    const Result<Weights> parsed = weightsFromJson(body);

    ASSERT_TRUE(parsed.ok());
    EXPECT_DOUBLE_EQ(5.0, parsed.value().importance);
    EXPECT_DOUBLE_EQ(0.0, parsed.value().blocks);
    EXPECT_DOUBLE_EQ(14.0, parsed.value().urgency_horizon_days);
}

TEST(WeightsTest, OmittedKeysKeepTheirDefaults)
{
    // A subset is a legal request: the omitted terms are left at the documented
    // defaults. Pinning this here keeps a later "strict schema" refactor from
    // turning the documented partial update into an error.
    const nlohmann::json body{ { "urgency", 12.5 } };

    const Result<Weights> parsed = weightsFromJson(body);

    ASSERT_TRUE(parsed.ok());
    EXPECT_DOUBLE_EQ(12.5, parsed.value().urgency);
    EXPECT_DOUBLE_EQ(5.0, parsed.value().importance);
    EXPECT_DOUBLE_EQ(2.0, parsed.value().age);
    EXPECT_DOUBLE_EQ(8.0, parsed.value().blocks);
    EXPECT_DOUBLE_EQ(7.0, parsed.value().urgency_horizon_days);
}

TEST(WeightsTest, RoundTripPreservesEveryValue)
{
    // Values are chosen to be exactly representable in binary, so the comparison
    // can be exact: the whole point is that serialization loses nothing, and an
    // epsilon comparison would hide a lossy step.
    Weights original;
    original.importance = 1.5;
    original.urgency = 0.25;
    original.age = 8.0;
    original.blocks = 2.5;
    original.urgency_horizon_days = 0.5;

    const Result<Weights> decoded = weightsFromJson(toJson(original));

    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(original.importance, decoded.value().importance);
    EXPECT_EQ(original.urgency, decoded.value().urgency);
    EXPECT_EQ(original.age, decoded.value().age);
    EXPECT_EQ(original.blocks, decoded.value().blocks);
    EXPECT_EQ(original.urgency_horizon_days, decoded.value().urgency_horizon_days);

    // And structurally identical, which also catches a field silently dropped
    // from toJson(): the per-field checks above would be comparing defaults then.
    EXPECT_EQ(toJson(original), toJson(decoded.value()));
}

TEST(WeightsTest, RoundTripThroughDumpedTextPreservesEveryValue)
{
    // The real path is JSON over a socket and through a TEXT column, so the
    // values must survive the text encoding too, at the default dump precision.
    Weights original;
    original.importance = 4.0;
    original.urgency = 13.5;
    original.age = 1.25;
    original.blocks = 3.0;
    original.urgency_horizon_days = 2.5;

    const nlohmann::json reparsed = nlohmann::json::parse(toJson(original).dump());
    const Result<Weights> decoded = weightsFromJson(reparsed);

    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(toJson(original), toJson(decoded.value()));
}

TEST(WeightsTest, WeightAtTheUpperBoundSurvivesARoundTrip)
{
    // The bound is a legal value, so it has to survive toJson -> weightsFromJson
    // like any other legal weight, including through dumped text — the real
    // path over the socket and the settings table. A ceiling the serializer
    // could not carry back would make the largest legal tuning unsettable.
    Weights original;
    original.importance = kMaxWeight;
    original.urgency = kMaxWeight;
    original.age = kMaxWeight;
    original.blocks = kMaxWeight;

    const nlohmann::json reparsed = nlohmann::json::parse(toJson(original).dump());
    const Result<Weights> decoded = weightsFromJson(reparsed);

    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(original.importance, decoded.value().importance);
    EXPECT_EQ(original.urgency, decoded.value().urgency);
    EXPECT_EQ(original.age, decoded.value().age);
    EXPECT_EQ(original.blocks, decoded.value().blocks);
    EXPECT_EQ(original.urgency_horizon_days, decoded.value().urgency_horizon_days);

    // Structurally identical too, which catches a field dropped from toJson().
    EXPECT_EQ(toJson(original), toJson(decoded.value()));
}

} // namespace taskpilot
