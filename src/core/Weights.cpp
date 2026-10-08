// Weights.cpp — validation and JSON round-trip for the ranking coefficients.
//
// Weights are data, not code: they live in the store's settings table and are
// retuned at runtime over MCP (set_weights), so this is the one part of the
// scoring math that reaches the engine as bytes from outside. Everything here
// exists to keep an invalid value from ever reaching the formula.
//
// Why the checks are strict rather than forgiving (matching Weights.hpp):
//   1. A NaN weight poisons every comparison. NaN compares false against
//      everything, itself included, so a score built from it is unordered and
//      the queue silently reshuffles between calls — nothing throws, nothing
//      logs, and the only symptom is a queue that makes no sense.
//   2. A negative weight inverts the meaning of its term: aging would punish
//      old tasks, "blocks" would push work that unblocks others to the bottom.
//      No legitimate tuning wants that, so a negative is far more likely a typo
//      (a stray minus in a hand-written patch) than an intent, and obeying it
//      would quietly destroy the queue order.
//   3. urgency = remaining / horizon, so a zero horizon divides by zero and a
//      negative one makes urgency grow as a deadline recedes.
//   4. A weight above kMaxWeight is finite but still too big: the score that
//      reaches the wire is the weight's PRODUCT with a task field, and a
//      product that overflows to +inf serializes as JSON null. See that
//      constant for the full reasoning.
// Because every failure names the offending field and its value, a caller can
// fix the request in one round trip instead of bisecting it field by field.

#include "core/Weights.hpp"

#include <cmath>
#include <sstream>
#include <string>

namespace taskpilot
{

namespace
{

/// One tunable field of Weights. Both validate() and weightsFromJson() walk
/// this list, so adding a sixth weight means adding one row here rather than
/// remembering to update two switch statements — which is exactly how a field
/// ends up parsed on one path and unchecked on the other.
struct FieldRef
{
    const char *key;         ///< JSON key, and the name used in error messages.
    double Weights::*member; ///< Which field of Weights to read or write.
    bool strictly_positive;  ///< True only for the horizon; 0 is legal for a weight.
};

/// Fixed order, so the same invalid set always reports the same field first: a
/// user retrying a corrected request, and a test asserting on the message,
/// should not see the complaint wander between fields.
constexpr FieldRef kFields[] = {
    { "importance", &Weights::importance, false },
    { "urgency", &Weights::urgency, false },
    { "age", &Weights::age, false },
    { "blocks", &Weights::blocks, false },
    { "urgency_horizon_days", &Weights::urgency_horizon_days, true },
};

/// Render a double for an error message using the stream's default formatting
/// (6 significant digits). The caller needs to recognise the value it sent —
/// "-1", "0", "1e+30" — not its exact bits, and a short message keeps the error
/// readable in a REPL line or a JSON-RPC error object.
std::string numberForMessage(double value)
{
    std::ostringstream out;
    out << value;
    return out.str();
}

/// Build "weights.<key> must be <rule> (got <value>)".
std::string fieldError(const char *key, const char *rule, double value)
{
    return std::string{ "weights." } + key + " must be " + rule + " (got " +
           numberForMessage(value) + ")";
}

} // namespace

Status Weights::validate() const
{
    // One code path for all five fields, with the per-field exceptions carried
    // in the table entry (the horizon is the only field that differs):
    //
    // 1. Finiteness is checked first and on its own. It has to precede the
    //    range check because NaN compares false against every bound, so a naive
    //    "value < 0" would answer false for the most dangerous value of all and
    //    let it through to the scoring math.
    // 2. Then the range: a weight may be >= 0 (0.0 legitimately switches a term
    //    off), while the horizon must be > 0 because urgency divides by it.
    // 3. Then the ceiling on a weight. It comes last because it only matters
    //    for a value the two checks above have already let through: finiteness
    //    is about the weight alone, the ceiling is about the PRODUCT that
    //    reaches the wire. The horizon is exempt by design (see Weights.hpp):
    //    it divides into the urgency ramp instead of multiplying into the
    //    score, so no horizon, however large, can make a score non-finite.
    for (const FieldRef &field : kFields)
    {
        const double value = this->*(field.member);

        if (!std::isfinite(value))
        {
            return Error::invalidArgument(fieldError(field.key, "finite", value));
        }

        if (field.strictly_positive)
        {
            if (0.0 >= value)
            {
                return Error::invalidArgument(fieldError(field.key, "> 0", value));
            }
            continue;
        }

        if (0.0 > value)
        {
            return Error::invalidArgument(fieldError(field.key, ">= 0", value));
        }

        // The bound is spelled "1e9" here, exactly as kMaxWeight and
        // docs/scoring.md spell it, rather than restreamed through
        // numberForMessage() (which would print "1e+09"): a caller comparing
        // the error against the documentation should not have to translate
        // between two spellings of the same number.
        if (kMaxWeight < value)
        {
            return Error::invalidArgument(fieldError(field.key, "<= 1e9", value));
        }
    }

    return Unit{};
}

nlohmann::json toJson(const Weights &weights)
{
    // Every field is written, including ones still at their default, so a client
    // reading a weights object sees the whole tuning in one place instead of
    // having to know which keys this build chose to omit.
    //
    // Note that nlohmann serializes a non-finite double as null: there is no NaN
    // literal in JSON. validate() is what keeps such a struct off the wire, and
    // weightsFromJson() rejects the null again on the way back in, so a non-finite
    // value cannot round-trip silently in either direction.
    return nlohmann::json{
        { "importance", weights.importance },
        { "urgency", weights.urgency },
        { "age", weights.age },
        { "blocks", weights.blocks },
        { "urgency_horizon_days", weights.urgency_horizon_days },
    };
}

Result<Weights> weightsFromJson(const nlohmann::json &json)
{
    // The top level has to be an object. Anything else — a bare number, null, a
    // discarded value left over from a failed parse — carries no field names, so
    // it cannot be a partially-specified tuning: only a caller bug. Report it
    // rather than inventing defaults the caller never asked for.
    if (!json.is_object())
    {
        return Error::invalidArgument(std::string{ "weights must be a JSON object (got " } +
                                      json.type_name() + ")");
    }

    // Start from the documented defaults and overwrite only the keys present, so
    // a caller can send the full object (the round trip of toJson) or a subset.
    // Omitting a key means "leave it at the default" and is not an error, but a
    // key that IS present and invalid must fail loudly — see the strictness note
    // at the top of this file.
    //
    // Merging a partial patch with the CURRENT weights is the caller's job: this
    // function has no store to read them from, and guessing on its behalf would
    // silently reset the terms the caller did not mention.
    Weights weights;

    for (const FieldRef &field : kFields)
    {
        if (!json.contains(field.key))
        {
            continue;
        }
        const nlohmann::json &value = json.at(field.key);

        // Integers and floats are both numbers: a client written in a language
        // with no float type, or a hand-written "importance": 5, must not be
        // rejected for spelling the same value differently. Every other JSON
        // type — null, string, boolean, array, object — is a wrong type here.
        if (!value.is_number())
        {
            return Error::invalidArgument(std::string{ "weights." } + field.key +
                                          " must be a number (got " + value.type_name() + ")");
        }

        weights.*(field.member) = value.get<double>();
    }

    // The range and finiteness rules live in validate() so a struct assembled by
    // hand and one parsed from JSON are held to exactly the same contract.
    const Status valid = weights.validate();
    if (!valid.ok())
    {
        return valid.error();
    }

    return weights;
}

} // namespace taskpilot
