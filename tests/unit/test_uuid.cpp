// tests/unit/test_uuid.cpp — uuid generation, text-derived uuids, and the shape
// check
//
// The generator and the checker face different accidents, so they are tested
// against different things:
//
//   - The generator's failure mode is SILENT REPETITION. A repeated uuid breaks
//     nothing at creation time; it breaks a sync, much later, when two
//     machines' exports collide on one uid and a merge overwrites one task with
//     another. Ten thousand draws with no repeat is the cheapest test that can
//     see it: a generator seeded from a constant, a clock, or a buffer that was
//     never filled produces its first repeat almost immediately at this volume,
//     whereas a real 122-bit generator could not collide even once in any
//     number of draws this test could run — so a repeat here means a broken
//     generator, never bad luck.
//   - The checker is the gate that stops a truncated or hand-edited export line
//     from reaching a merge, so its cases are the near-misses a hand edit and a
//     bad checkout actually produce: one character short, one hyphen moved, a
//     version nibble typed as 1 instead of 4.
//
// Text case is a deliberate decision rather than an accident: the generator
// emits lowercase and the checker ACCEPTS LOWERCASE ONLY, because every merge
// comparison keys on the uid string and two spellings are two identities. The
// reasoning, and what it costs, are in UuidShapeTest.UppercaseHexIsRejected
// below — a deliberate reversal of what this file used to assert.
//
// deriveUuidFromText faces a third accident: two machines that hold the same
// rows must derive the SAME uid for them, or the first sync duplicates every
// task. Its tests are therefore about determinism and reach rather than about
// randomness — the same canonical text always yields the same uuid, similar
// texts do not collide, and the result passes the checker the rest of the
// system gates on (UuidDerivationTest at the end of this file).

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/Uuid.hpp"

namespace
{

/// A known-good v4 uuid. Every mutation case below is derived from it, so each
/// one differs from a valid value in exactly the way its name says — a case
/// that failed for two reasons at once would say nothing about either.
constexpr const char *kCanonicalUuid = "9f3c1a2b-4d5e-4f60-8a9b-0c1d2e3f4a5b";

/// The canonical uuid with its digits replaced by 'x'. Comparing a generated
/// uuid against this checks the LAYOUT without asking looksLikeUuidV4, which is
/// itself under test here — a test that used the checker to validate the
/// generator would agree with itself even if the two were wrong together.
constexpr const char *kCanonicalSkeleton = "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx";

/// The length of every valid text form: 32 hex digits plus 4 hyphens.
constexpr std::size_t kUuidTextLength{ 36 };

/// Offsets of the two nibbles that carry the version and the variant. Named
/// because the digits on their own are indistinguishable from any other offset
/// in a 36-character string.
constexpr std::size_t kVersionIndex{ 14 };
constexpr std::size_t kVariantIndex{ 19 };

/// Offsets of the four hyphens in the canonical form, so the position cases can
/// move one without spelling the number twice.
constexpr std::size_t kFirstHyphen{ 8 };
constexpr std::size_t kSecondHyphen{ 13 };
constexpr std::size_t kThirdHyphen{ 18 };
constexpr std::size_t kFourthHyphen{ 23 };

/// kCanonicalSkeleton as an object, so a loop can ask "is this offset a
/// hyphen?" without repeating the layout arithmetic.
const std::string kSkeleton{ kCanonicalSkeleton };

/// The canonical uuid with one character replaced.
[[nodiscard]] std::string withCharacterAt(std::size_t index, char replacement)
{
    std::string mutated{ kCanonicalUuid };
    mutated[index] = replacement;
    return mutated;
}

/// The canonical uuid with the characters at `first` and `second` exchanged.
///
/// Swapping rather than deleting keeps the result 36 characters long, so a case
/// built this way can only fail the hyphen-POSITION rule. Deleting the hyphen
/// would trip the length rule first, and the two rules would become
/// indistinguishable in the failure output.
[[nodiscard]] std::string withCharactersSwapped(std::size_t first, std::size_t second)
{
    std::string mutated{ kCanonicalUuid };
    std::swap(mutated[first], mutated[second]);
    return mutated;
}

/// True for the four characters RFC 4122 allows in the variant nibble, spelled
/// the way the generator is required to emit them.
[[nodiscard]] bool isLowercaseVariantNibble(char character)
{
    return '8' == character || '9' == character || 'a' == character || 'b' == character;
}

/// True for a hex digit spelled the way the text form requires: 0-9 or a-f.
///
/// Spelled out here rather than borrowed from Uuid.cpp on purpose — a test that
/// judged the output with the implementation's own classifier could never
/// disagree with it, which is exactly when a test is worth having.
[[nodiscard]] bool isLowercaseHexDigit(char character)
{
    return ('0' <= character && '9' >= character) || ('a' <= character && 'f' >= character);
}

/// The canonical text the derivation tests feed to deriveUuidFromText. Tab
/// separated and fixed in its field order, which is the caller's half of the
/// contract: the function is deterministic over the bytes it is handed, and
/// canonicalisation belongs to whoever builds the row's text.
constexpr const char *kCanonicalRowText = "42\tclean the garage\t2026-10-08T09:00:00Z\t7";

} // namespace

TEST(UuidGenerationTest, ProducesTheCanonicalShape)
{
    const taskpilot::Result<std::string> generated = taskpilot::generateUuidV4();

    // A failure here is a genuine failure of the environment and not a flaky
    // test: /dev/urandom exists on every platform taskPilot supports, and the
    // header deliberately provides no weaker source to fall back to.
    ASSERT_TRUE(generated.ok()) << generated.error().message;

    const std::string &uuid = generated.value();

    // The skeleton is 36 characters long by construction, so this assertion
    // pins the length AND keeps the offset loop below in bounds.
    ASSERT_EQ(kSkeleton.size(), uuid.size());
    EXPECT_EQ(kUuidTextLength, uuid.size());
    for (std::size_t index = 0; index < kSkeleton.size(); ++index)
    {
        if ('-' == kSkeleton[index])
        {
            EXPECT_EQ('-', uuid[index]) << "missing hyphen at offset " << index;
        }
        else
        {
            EXPECT_NE('-', uuid[index]) << "unexpected hyphen at offset " << index;
        }
    }

    // The two fixed nibbles: the whole difference between a v4 uuid and any
    // other 36-character hex string with hyphens in the right places.
    EXPECT_EQ('4', uuid[kVersionIndex]);
    EXPECT_TRUE(isLowercaseVariantNibble(uuid[kVariantIndex]))
        << "variant nibble is " << uuid[kVariantIndex];
}

TEST(UuidGenerationTest, TenThousandUuidsAreDistinctAndWellFormed)
{
    constexpr int kSampleSize{ 10000 };

    // Counters rather than an assertion per draw: at this sample size a broken
    // generator would otherwise print ten thousand identical lines and bury the
    // one fact that matters, so the loop records the first offending value.
    std::unordered_set<std::string> seen;
    std::size_t malformed{ 0 };
    std::size_t duplicates{ 0 };
    std::string firstMalformed;
    std::string firstDuplicate;

    for (int draw = 0; draw < kSampleSize; ++draw)
    {
        const taskpilot::Result<std::string> generated = taskpilot::generateUuidV4();
        ASSERT_TRUE(generated.ok()) << "draw " << draw << ": " << generated.error().message;

        const std::string &uuid = generated.value();
        if (!taskpilot::looksLikeUuidV4(uuid))
        {
            ++malformed;
            if (firstMalformed.empty())
            {
                firstMalformed = uuid;
            }
        }
        if (!seen.insert(uuid).second)
        {
            ++duplicates;
            if (firstDuplicate.empty())
            {
                firstDuplicate = uuid;
            }
        }
    }

    EXPECT_EQ(std::size_t{ 0 }, malformed) << "first malformed uuid: " << firstMalformed;
    EXPECT_EQ(std::size_t{ 0 }, duplicates) << "first repeated uuid: " << firstDuplicate;
    EXPECT_EQ(static_cast<std::size_t>(kSampleSize), seen.size());
}

TEST(UuidGenerationTest, EmitsLowercaseHexOnly)
{
    // The generator's case is an interface, not a detail: the export is a set
    // of lines for git to merge, and two machines that spelled one uid
    // differently would look like two uids, breaking the "exactly one line per
    // uid" property the whole format rests on. Together with
    // UuidShapeTest.UppercaseHexIsRejected below, this test says the spelling
    // is unique end to end: everything taskPilot writes is lowercase, and
    // everything it reads back must be lowercase too.
    constexpr int kSampleSize{ 100 };
    bool sawLetter{ false };

    for (int draw = 0; draw < kSampleSize; ++draw)
    {
        const taskpilot::Result<std::string> generated = taskpilot::generateUuidV4();
        ASSERT_TRUE(generated.ok()) << "draw " << draw << ": " << generated.error().message;

        for (const char character : generated.value())
        {
            if ('a' <= character && 'f' >= character)
            {
                sawLetter = true;
            }
            EXPECT_FALSE('A' <= character && 'F' >= character)
                << "uppercase " << character << " in " << generated.value();
        }
    }

    // The loop can only judge the letters it actually saw, so require that it
    // saw some: a run in which every uuid happened to be all digits would
    // otherwise leave the case check vacuous.
    EXPECT_TRUE(sawLetter) << "no hex letter in " << kSampleSize << " uuids";
}

TEST(UuidGenerationTest, EveryRandomHexPositionVariesBetweenCalls)
{
    // Distinctness alone cannot see a PARTIALLY filled buffer. A generator that
    // read eight bytes and left the other eight at their initial value would
    // still be very unlikely to collide at the sample size above, yet half of
    // every uuid it produced would be identical to the last one. Sampling each
    // hex position and requiring two different values in each one catches
    // exactly that, and catches a constant or clock-derived byte slipped into
    // any single position.
    //
    // The two positions that carry the version and the variant are excluded
    // from the variation requirement — they are fixed by the format — and
    // asserted to hold the one value the format allows instead. That is what
    // makes this test able to see the stamping step being skipped: an unstamped
    // generator leaves random nibbles in those offsets, and the sets below
    // would then hold far more than '4' or more than one variant nibble.
    constexpr int kSampleSize{ 64 };
    std::array<std::set<char>, kUuidTextLength> observedAt{};

    for (int draw = 0; draw < kSampleSize; ++draw)
    {
        const taskpilot::Result<std::string> generated = taskpilot::generateUuidV4();
        ASSERT_TRUE(generated.ok()) << "draw " << draw << ": " << generated.error().message;
        ASSERT_EQ(kUuidTextLength, generated.value().size());

        for (std::size_t offset = 0; offset < kUuidTextLength; ++offset)
        {
            const char character = generated.value()[offset];
            if ('-' == character)
            {
                continue;
            }
            observedAt[offset].insert(character);
        }
    }

    for (std::size_t offset = 0; offset < kUuidTextLength; ++offset)
    {
        if ('-' == kSkeleton[offset])
        {
            continue; // Not a hex position, so there is nothing to vary.
        }
        if (kVersionIndex == offset)
        {
            const std::set<char> expected{ '4' };
            EXPECT_EQ(expected, observedAt[offset])
                << "offset " << offset << " is not always the v4 version nibble";
            continue;
        }
        if (kVariantIndex == offset)
        {
            for (const char character : observedAt[offset])
            {
                EXPECT_TRUE(isLowercaseVariantNibble(character))
                    << "offset " << offset << " held " << character;
            }
            continue;
        }
        EXPECT_GE(observedAt[offset].size(), std::size_t{ 2 })
            << "hex offset " << offset << " never varied between calls";
    }
}

TEST(UuidShapeTest, AcceptsTheCanonicalForm)
{
    EXPECT_TRUE(taskpilot::looksLikeUuidV4(kCanonicalUuid));

    // The degenerate and the extreme ends of the range are both valid SHAPES.
    // Rejecting them would be the checker second-guessing uniqueness, which the
    // header says is the store's question and not this function's.
    EXPECT_TRUE(taskpilot::looksLikeUuidV4("00000000-0000-4000-8000-000000000000"));
    EXPECT_TRUE(taskpilot::looksLikeUuidV4("ffffffff-ffff-4fff-bfff-ffffffffffff"));
}

TEST(UuidShapeTest, RejectsEmptyAndWrongLengthForms)
{
    const std::string canonical{ kCanonicalUuid };

    const std::vector<std::string> rejected{
        "",                                 // a missing value
        canonical.substr(0, 35),            // truncated by one character
        canonical + "a",                    // one character too many
        canonical + "\n",                   // a JSONL line's newline left attached
        "9f3c1a2b4d5e4f608a9b0c1d2e3f4a5b", // the 32-digit form, hyphens dropped
        "{" + canonical + "}",              // the braced form
        "9f3c1a2b-4d5e-4f60-8a9b-0c1d2e3f4a5", // truncated inside the last group
        "  " + canonical,                   // leading padding, so it is 38 long
    };

    for (const std::string &text : rejected)
    {
        EXPECT_FALSE(taskpilot::looksLikeUuidV4(text))
            << "unexpectedly accepted: [" << text << "]";
    }
}

TEST(UuidShapeTest, RejectsHyphensInTheWrongPositions)
{
    std::vector<std::string> rejected{
        // Moving a hyphen one place, in both directions. None of the four
        // touches the version or the variant nibble, and the length stays 36,
        // so the position rule is the only one these can fail.
        withCharactersSwapped(kFirstHyphen, kFirstHyphen + 1),
        withCharactersSwapped(kSecondHyphen - 1, kSecondHyphen),
        withCharactersSwapped(kThirdHyphen, kThirdHyphen + 1),
        withCharactersSwapped(kFourthHyphen - 1, kFourthHyphen),
        // The other direction: a hyphen replaced by a digit, which is what a
        // mangled copy leaves behind. Still 36 characters, so again only the
        // position rule can catch it — and it has to, because a checker that
        // only complained about stray hyphens would read this line as a valid
        // 32-digit value and merge it under a uid nobody ever generated.
        withCharacterAt(kFirstHyphen, '0'),
        withCharacterAt(kSecondHyphen, '1'),
        withCharacterAt(kThirdHyphen, '2'),
        withCharacterAt(kFourthHyphen, '3'),
    };

    for (const std::string &text : rejected)
    {
        EXPECT_FALSE(taskpilot::looksLikeUuidV4(text))
            << "unexpectedly accepted: [" << text << "]";
    }
}

TEST(UuidShapeTest, RejectsAVersionOtherThanFour)
{
    // Every one of these is a well-formed uuid of some OTHER version: v1 is
    // time-based, v3 and v5 are name-based. They are what a file written by a
    // different tool would contain, and the export's contract is v4 — accepting
    // them would mean merging identifiers whose uniqueness this project never
    // established.
    const std::string wrongVersions{ "012356789abcdef" };

    for (const char digit : wrongVersions)
    {
        const std::string text = withCharacterAt(kVersionIndex, digit);
        EXPECT_FALSE(taskpilot::looksLikeUuidV4(text))
            << "unexpectedly accepted version nibble " << digit << ": " << text;
    }

    // The same position with the correct digit is accepted, so the cases above
    // cannot be passing by rejecting the string for some other reason.
    EXPECT_TRUE(taskpilot::looksLikeUuidV4(withCharacterAt(kVersionIndex, '4')));
}

TEST(UuidShapeTest, RejectsAVariantNibbleOutsideTheRfc4122Range)
{
    // The nibble's leading bits select the uuid scheme: 10xx is RFC 4122, so
    // only 8, 9, a and b describe the kind of uuid an export may carry.
    const std::string nonRfc4122{ "01234567cdef" };
    for (const char digit : nonRfc4122)
    {
        const std::string text = withCharacterAt(kVariantIndex, digit);
        EXPECT_FALSE(taskpilot::looksLikeUuidV4(text))
            << "unexpectedly accepted variant nibble " << digit << ": " << text;
    }

    const std::string rfc4122{ "89ab" };
    for (const char digit : rfc4122)
    {
        const std::string text = withCharacterAt(kVariantIndex, digit);
        EXPECT_TRUE(taskpilot::looksLikeUuidV4(text))
            << "unexpectedly rejected variant nibble " << digit << ": " << text;
    }
}

TEST(UuidShapeTest, RejectsNonHexCharacters)
{
    // 'g' and 'G' are the near-misses a hand edit produces (the alphabet stops
    // at f); the rest are the separators a copy-paste leaves behind. The last
    // entry is a byte that is not ASCII at all, included because the check must
    // classify BYTES: a multi-byte character occupying one of the 36 positions
    // is not a hex digit, whatever it renders as.
    const std::vector<char> rejected{
        'g',
        'z',
        'G',
        'Z',
        ' ',
        '_',
        '.',
        ':',
        '@',
        '!',
        static_cast<char>(0xC3),
    };

    for (const char character : rejected)
    {
        // Offset 2 is a hex position — not a hyphen, and neither the version
        // nor the variant — so each case fails the hex rule and nothing else.
        const std::string text = withCharacterAt(2, character);
        EXPECT_FALSE(taskpilot::looksLikeUuidV4(text))
            << "unexpectedly accepted character [" << character << "]";
    }
}

TEST(UuidShapeTest, UppercaseHexIsRejected)
{
    // DECISION REVERSED: this test used to be UppercaseHexIsAccepted, and it is
    // inverted rather than deleted so the reversal stays visible to whoever
    // reads the suite next.
    //
    // Why reject: every merge comparison keys on the uid STRING — the tasks.uid
    // column with its unique index, and the lookup in mergeJsonl — so "9F3C…"
    // and "9f3c…" are not two spellings of one identity, they are two
    // identities. A checker that took both would let one task be inserted
    // beside its own capitalised copy on every machine, and the shared export
    // file would carry both spellings forever. Nothing inside taskPilot writes
    // uppercase and nothing anywhere normalises case, so the lenient form had
    // no producer it was serving.
    //
    // What it costs, stated plainly: a uid capitalised in transit — by a hand
    // edit, or by a tool that is not taskPilot — is now refused outright
    // instead of merged, and the refusal names the record and stops a merge
    // that would otherwise have gone through. That is the intended trade: a
    // loud refusal is recoverable, a silent duplicate is not.
    EXPECT_FALSE(taskpilot::looksLikeUuidV4("9F3C1A2B-4D5E-4F60-8A9B-0C1D2E3F4A5B"));
    EXPECT_FALSE(taskpilot::looksLikeUuidV4("9f3c1a2b-4d5e-4f60-8A9B-0c1d2e3f4a5b"));

    // The variant nibble is where uppercase was most defensible — A and B name
    // the same nibbles as a and b — so it is pinned on its own rather than left
    // to the body's rule.
    EXPECT_FALSE(taskpilot::looksLikeUuidV4(withCharacterAt(kVariantIndex, 'A')));
    EXPECT_FALSE(taskpilot::looksLikeUuidV4(withCharacterAt(kVariantIndex, 'B')));

    // Every uppercase letter, at a body position, and every lowercase one as
    // the control: without the second loop the first would also pass if a hex
    // LETTER were rejected for some reason unrelated to its case.
    for (int code = 'A'; code <= 'F'; ++code)
    {
        const std::string text = withCharacterAt(2, static_cast<char>(code));
        EXPECT_FALSE(taskpilot::looksLikeUuidV4(text))
            << "unexpectedly accepted uppercase digit: " << text;
    }
    for (int code = 'a'; code <= 'f'; ++code)
    {
        const std::string text = withCharacterAt(2, static_cast<char>(code));
        EXPECT_TRUE(taskpilot::looksLikeUuidV4(text))
            << "unexpectedly rejected lowercase digit: " << text;
    }

    // This function IS the export parser's gate, not a second rule that might
    // drift from it: TaskSync::parseJsonLine and the merge validation in
    // TaskStore both refuse a record whose uid this function rejects, so a
    // capitalised line cannot reach a merge, let alone a row. The assertion
    // below is therefore the parser's own verdict, and the line after it is the
    // guarantee that lowercasing the same uid still merges as one identity.
    const std::string capitalised{ "9F3C1A2B-4D5E-4F60-8A9B-0C1D2E3F4A5B" };
    const std::string lowercase{ "9f3c1a2b-4d5e-4f60-8a9b-0c1d2e3f4a5b" };
    EXPECT_FALSE(taskpilot::looksLikeUuidV4(capitalised));
    EXPECT_TRUE(taskpilot::looksLikeUuidV4(lowercase));
}

TEST(UuidShapeTest, TheVerdictIsTheSameEveryTime)
{
    // Uuid.hpp calls the check pure: no store lookup, no cached state, no
    // clock. Repeatability is the only consequence of that claim a caller can
    // test from outside, and it is the one that matters — a checker that
    // mutated its argument or consulted a counter would answer differently on
    // the second call. Both verdicts are represented so both branches of the
    // function are covered.
    const std::vector<std::string> inputs{
        kCanonicalUuid,
        "",
        "9F3C1A2B-4D5E-4F60-8A9B-0C1D2E3F4A5B",
        withCharacterAt(kVersionIndex, '1'),
        withCharacterAt(kVariantIndex, 'c'),
        withCharactersSwapped(kFirstHyphen, kFirstHyphen + 1),
    };

    for (const std::string &text : inputs)
    {
        const bool first = taskpilot::looksLikeUuidV4(text);
        const bool second = taskpilot::looksLikeUuidV4(text);
        EXPECT_EQ(first, second) << "verdict changed for [" << text << "]";
    }
}

TEST(UuidDerivationTest, IsDeterministicForTheSameText)
{
    // Determinism is the whole contract. Two machines that restored the same
    // backup hold the SAME rows, and each derives the uid for those rows by
    // hashing them; if the derivation varied between calls, each machine would
    // assert a different uid for one task, the merge would see two unknown uids
    // and insert both — the duplication the derivation exists to prevent.
    const std::string canonical{ kCanonicalRowText };

    const std::string first = taskpilot::deriveUuidFromText(canonical);
    const std::string second = taskpilot::deriveUuidFromText(canonical);
    EXPECT_EQ(first, second) << "repeated derivation disagreed: " << first << " vs " << second;

    // The same CONTENT reached through a different string object, so a
    // derivation that hashed the argument itself — a pointer, an address, a
    // cached iterator — rather than the bytes it points at would fail here even
    // though the two calls above agree.
    std::string rebuilt{ "42\tclean the garage" };
    rebuilt.append("\t2026-10-08T09:00:00Z\t7");
    EXPECT_EQ(first, taskpilot::deriveUuidFromText(rebuilt));
}

TEST(UuidDerivationTest, DifferentTextsDeriveDifferentUuids)
{
    // Deliberately similar inputs: the collisions that matter are between rows
    // that differ by one character (a typo, an edit), by length (a field that
    // gained a digit), or by case — not between unrelated strings, which any
    // digest separates. The empty string is included because it is the boundary
    // a row with no fields at all produces.
    const std::vector<std::string> texts{
        "",
        "a",
        "b",
        "aa",
        "ab",
        "clean the garage",
        "clean the garage ",
        "clean the garagE",
        "42\tclean the garage",
        "43\tclean the garage",
        std::string(64, 'x'),
        std::string(65, 'x'),
    };

    std::set<std::string> derived;
    for (const std::string &text : texts)
    {
        const std::string uuid = taskpilot::deriveUuidFromText(text);
        EXPECT_TRUE(derived.insert(uuid).second)
            << "two canonical forms derived one uid " << uuid << "; the second is [" << text << "]";
    }
    EXPECT_EQ(texts.size(), derived.size());

    // And the two 64-bit passes must be genuinely separate. Were both of them
    // run with the same constants, the second eight bytes would be the first
    // eight over again — byte i would equal byte i + 8 throughout — so the
    // identifier would carry about 64 bits of content while Uuid.hpp's
    // collision argument rests on 128.
    //
    // The comparison therefore takes only digits that are unstamped in BOTH
    // halves: bytes 1 to 5 (digits 2 to 11) against bytes 9 to 13 (digits 18 to
    // 27), five bytes in each. The version nibble overwrites byte 6's high
    // nibble and the variant overwrites byte 8's top two bits, so comparing
    // whole halves — or whole six-byte prefixes — would pass either way. Five
    // bytes is the strongest clean invariant: a correct derivation matches them
    // as a 2^-40 event, so over this sample a match says the two passes share
    // their constants, not that this run was unlucky.
    constexpr std::size_t kUnstampedOffset{ 2 };
    constexpr std::size_t kUnstampedDigits{ 10 };
    for (const std::string &text : texts)
    {
        const std::string uuid = taskpilot::deriveUuidFromText(text);
        std::string digits;
        for (const char character : uuid)
        {
            if ('-' != character)
            {
                digits.push_back(character);
            }
        }
        ASSERT_EQ(std::size_t{ 32 }, digits.size());
        EXPECT_NE(digits.substr(kUnstampedOffset, kUnstampedDigits),
                  digits.substr(16 + kUnstampedOffset, kUnstampedDigits))
            << "the two digest passes repeat one value: " << uuid;
    }
}

TEST(UuidDerivationTest, OutputIsAWellFormedLowercaseUuid)
{
    // The v4 shape is what keeps the rest of the system single-path: the export
    // parser, the merge lookup and the store's uid column all gate on
    // looksLikeUuidV4, so a derived identifier that failed it would need a
    // second branch everywhere it is read.
    const std::vector<std::string> texts{
        "",
        "a",
        kCanonicalRowText,
        std::string(4096, 'q'), // a long canonical form, so length is not special
    };

    for (const std::string &text : texts)
    {
        const std::string uuid = taskpilot::deriveUuidFromText(text);
        EXPECT_EQ(kUuidTextLength, uuid.size());
        EXPECT_TRUE(taskpilot::looksLikeUuidV4(uuid)) << "derived uuid: " << uuid;

        // Lowercase is asserted on its own rather than left to the shape check:
        // the check's lowercase rule implies it today, and this is what keeps
        // the promise if that rule is ever loosened again.
        for (const char character : uuid)
        {
            if ('-' == character)
            {
                continue;
            }
            EXPECT_TRUE(isLowercaseHexDigit(character))
                << "derived uuid carries " << character << ": " << uuid;
        }

        // The two stamped nibbles, checked here so a change that stopped
        // stamping is named for what it is instead of surfacing as a generic
        // shape failure.
        EXPECT_EQ('4', uuid[kVersionIndex]) << "derived uuid: " << uuid;
        EXPECT_TRUE(isLowercaseVariantNibble(uuid[kVariantIndex]))
            << "derived variant nibble is " << uuid[kVariantIndex] << ": " << uuid;
    }
}

TEST(UuidDerivationTest, EmptyTextIsALegalInputWithAStableUuid)
{
    // A row whose fields are all empty canonicalises to the empty string, and a
    // migration may hand exactly that to the derivation. It is a legal input:
    // it must yield a stable, well-formed uuid rather than crashing or
    // producing something no reader accepts.
    const std::string derived = taskpilot::deriveUuidFromText("");

    EXPECT_EQ(kUuidTextLength, derived.size());
    EXPECT_TRUE(taskpilot::looksLikeUuidV4(derived)) << derived;
    EXPECT_EQ(derived, taskpilot::deriveUuidFromText("")) << "the empty input is not stable";

    // A zeroed buffer is also stable and well-formed, so only an explicit
    // comparison can see that shortcut: the value it produces is the nil uuid,
    // which is the placeholder a malformed record is likeliest to carry, and
    // two different things sharing one identity is the class of bug this file
    // exists to prevent.
    EXPECT_NE(std::string("00000000-0000-4000-8000-000000000000"), derived);
}

TEST(UuidDerivationTest, KnownTextKeepsItsKnownUuid)
{
    // A frozen test vector, pinned on purpose rather than because the value was
    // convenient. The derived uid is how EVERY machine names a pre-uid row, so
    // a change to a constant, to the byte layout or to the stamping makes a
    // restored backup derive different uids for rows that already exist —
    // every old task is then inserted twice, on every machine at once. No other
    // test in this suite can see that: a self-consistent change to the
    // derivation leaves all of them passing. If this ever fails, the fix is to
    // restore the derivation, never to re-record the value.
    const std::string canonical{ kCanonicalRowText };
    EXPECT_EQ(std::string("66bf21eb-e764-456c-88bc-8bf6e2a4be80"),
              taskpilot::deriveUuidFromText(canonical));
}
