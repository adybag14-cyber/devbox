#include "../src/research_signature.hpp"
#include "devbox/common.hpp"
#include <algorithm>
#include <iostream>
#include <random>
#include <re2/re2.h>
using namespace devbox;
using namespace devbox::web::detail;
// Retained pre-optimization algorithm is an independent compatibility oracle.
TextSignature reference_signature(std::string_view text) {
    TextSignature result;
    std::vector<std::string> words;
    const RE2 token("([\\p{L}\\p{N}][\\p{L}\\p{N}_-]*)");
    static const RE2 significant("(?i)(?:.*\\p{N}.*|no|not|never|without|нет|не|ни|никогда|без)");
    re2::StringPiece remaining(text.data(), text.size());
    std::string word;
    while (RE2::FindAndConsume(&remaining, token, &word)) {
        word = lower(word);
        if (RE2::FullMatch(word, significant))
            result.significant_tokens.push_back(word);
        words.push_back(std::move(word));
    }
    result.words = words.size();
    // This is a bounded text-similarity sketch, never a cryptographic identity or
    // an assertion of factual agreement. Every excluded document is retained.
    for (std::size_t i = 0; i + 5 <= words.size(); ++i) {
        std::uint64_t hash = 14695981039346656037ULL;
        for (std::size_t j = i; j < i + 5; ++j) {
            for (const unsigned char c : words[j])
                hash = (hash ^ c) * 1099511628211ULL;
            hash = (hash ^ 0xffU) * 1099511628211ULL;
        }
        result.shingles.push_back(hash);
    }
    std::sort(result.shingles.begin(), result.shingles.end());
    result.shingles.erase(std::unique(result.shingles.begin(), result.shingles.end()), result.shingles.end());
    if (result.shingles.size() > 2048) {
        result.shingles.resize(2048);
        result.sampled = true;
    }
    return result;
}
void check(std::string_view input) {
    const auto expected = reference_signature(input);
    const auto actual = content_signature(input);
    if (expected.words != actual.words || expected.sampled != actual.sampled ||
        expected.shingles != actual.shingles || expected.significant_tokens != actual.significant_tokens)
        throw Error("Streaming signature changed the retained full-word algorithm's output");
}
int main() {
    try {
        std::string small;
        check(small);
        for (unsigned n = 0; n < 12; ++n) {
            small += " word" + std::to_string(n);
            check(small);
        }
        const std::vector<std::string> vocabulary{"MEASURED",
                                                  "calibration",
                                                  "not",
                                                  "no",
                                                  "without",
                                                  "never",
                                                  "signal-123",
                                                  "123",
                                                  "\xce\xb1\xce\xb2",
                                                  "\xe4\xb8\xad",
                                                  "нет",
                                                  "НЕ",
                                                  "ни",
                                                  "никогда",
                                                  "без",
                                                  "PhotodetectorSpectrophotometerUncertainty",
                                                  "don't",
                                                  "x_y",
                                                  "\xf0\x9f\x93\xb1"};
        const std::vector<std::string> separators{" ", ", ", ".\n", "\t", "\xc2\xa0", "/"};
        std::mt19937 random(81723);
        for (unsigned fixture = 0; fixture < 40; ++fixture) {
            std::string text;
            for (unsigned n = 0; n < fixture * 13; ++n)
                text += vocabulary[random() % vocabulary.size()] + separators[random() % separators.size()];
            check(text);
        }
        std::string large;
        for (unsigned n = 0; n < 6000; ++n)
            large += "measurement-" + std::to_string(n) + " ";
        check(large);
        if (!content_signature(large).sampled || content_signature(large).shingles.size() != 2048)
            throw Error("Sampling retains the same 2048 smallest unique hashes");
        std::string report;
        for (unsigned n = 0; n < 100; ++n)
            report += "calibrated optics report with stable measurements ";
        const auto original = content_signature(report + " value 99 without correction");
        const auto numeric_change = content_signature(report + " value 199 without correction");
        const auto negation_change = content_signature(report + " value 99 with correction");
        if (similarity(original, original) != 1 || similarity(original, numeric_change) != 0 ||
            similarity(original, negation_change) != 0)
            throw Error("Numeric or negation changes must keep reports distinct");
        std::cout << "Research signature reference equality, sampling and significant facts passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
