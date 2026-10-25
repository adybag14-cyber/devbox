#include "devbox/fast_deflate.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <zlib.h>

namespace devbox {
namespace {
constexpr std::array<unsigned, 29> length_base{3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                                               15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                                               67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<unsigned, 29> length_extra{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<unsigned, 30> distance_base{
    1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr auto length_code = [] {
    std::array<unsigned char, 259> table{};
    unsigned code = 0;
    for (unsigned length = 3; length <= 258; ++length) {
        while (code + 1 < length_base.size() && length >= length_base[code + 1])
            ++code;
        table[length] = static_cast<unsigned char>(code);
    }
    return table;
}();
constexpr unsigned reverse(unsigned value, unsigned width) {
    unsigned result = 0;
    for (unsigned i = 0; i < width; ++i) {
        result = (result << 1) | (value & 1);
        value >>= 1;
    }
    return result;
}
struct Code {
    unsigned bits, width;
};
constexpr auto literals = [] {
    std::array<Code, 288> table{};
    for (unsigned symbol = 0; symbol < table.size(); ++symbol) {
        const auto width = symbol < 144 ? 8u : symbol < 256 ? 9u : symbol < 280 ? 7u : 8u;
        const auto code = symbol < 144   ? symbol + 0x30u
                          : symbol < 256 ? symbol - 144u + 0x190u
                          : symbol < 280 ? symbol - 256u
                                         : symbol - 280u + 0xc0u;
        table[symbol] = {reverse(code, width), width};
    }
    return table;
}();
class Writer {
    std::span<char> output_;
    std::size_t used_ = 0;
    std::uint64_t pending_ = 0;
    unsigned count_ = 0;
    bool full_ = false;

  public:
    explicit Writer(std::span<char> output) : output_(output) {}
    void byte(unsigned value) {
        if (used_ == output_.size()) {
            full_ = true;
            return;
        }
        output_[used_++] = static_cast<char>(value);
    }
    void bits(unsigned value, unsigned width) {
        if (full_)
            return;
        pending_ |= static_cast<std::uint64_t>(value) << count_;
        count_ += width;
        while (count_ >= 8) {
            byte(static_cast<unsigned>(pending_ & 0xffu));
            pending_ >>= 8;
            count_ -= 8;
        }
    }
    void symbol(unsigned value) {
        bits(literals[value].bits, literals[value].width);
    }
    void align() {
        if (count_)
            byte(static_cast<unsigned>(pending_ & 0xffu));
        pending_ = 0;
        count_ = 0;
    }
    void little(std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i)
            byte((value >> (8 * i)) & 0xffu);
    }
    bool full() const {
        return full_;
    }
    std::size_t size() const {
        return full_ ? 0 : used_;
    }
};
std::uint32_t word(const char* input) {
    std::uint32_t result;
    std::memcpy(&result, input, sizeof(result));
    return result;
}
unsigned hash(const char* input) {
    return (word(input) * 0x1e35a7bdu) >> 18;
}
unsigned distance_code(unsigned distance) {
    if (distance <= 4)
        return distance - 1u;
    const auto logarithm = static_cast<unsigned>(std::bit_width(distance - 1u) - 1);
    return 2u * logarithm + (((distance - 1u) >> (logarithm - 1u)) & 1u);
}
template <std::size_t N>
bool huffman(std::array<unsigned, N> frequencies, std::array<Code, N>& codes, unsigned maximum) {
    if (!maximum || maximum > 15)
        return false;
    struct Node {
        unsigned weight = 0, minimum = 0;
        int parent = -1;
    };
    std::array<Node, N * 2> nodes{};
    std::array<unsigned, N> heap{};
    std::array<int, N> leaves{};
    leaves.fill(-1);
    codes.fill({0, 0});
    unsigned active = 0;
    for (auto frequency : frequencies)
        active += frequency != 0;
    // A complete two-symbol tree also represents empty distance alphabets and
    // the single-symbol cases without relying on decoder-specific exceptions.
    for (unsigned symbol = 0; active < 2 && symbol < N; ++symbol)
        if (!frequencies[symbol]) {
            frequencies[symbol] = 1;
            ++active;
        }
    unsigned used = 0, count = 0;
    const auto less = [&](unsigned a, unsigned b) {
        const auto& left = nodes.at(a);
        const auto& right = nodes.at(b);
        return left.weight != right.weight ? left.weight < right.weight : left.minimum < right.minimum;
    };
    const auto push = [&](unsigned node) {
        if (count >= heap.size())
            throw std::logic_error("Native Huffman heap capacity");
        auto at = count++;
        while (at && less(node, heap.at((at - 1) / 2))) {
            heap.at(at) = heap.at((at - 1) / 2);
            at = (at - 1) / 2;
        }
        heap.at(at) = node;
    };
    const auto pop = [&] {
        if (!count || count > heap.size())
            throw std::logic_error("Native Huffman heap size");
        const auto first = heap[0], value = heap.at(--count);
        unsigned at = 0;
        while (at * 2 + 1 < count) {
            auto child = at * 2 + 1;
            if (child + 1 < count && less(heap.at(child + 1), heap.at(child)))
                ++child;
            if (!less(heap.at(child), value))
                break;
            heap.at(at) = heap.at(child);
            at = child;
        }
        if (count)
            heap.at(at) = value;
        return first;
    };
    for (unsigned symbol = 0; symbol < N; ++symbol)
        if (frequencies[symbol]) {
            nodes.at(used) = {frequencies[symbol], symbol, -1};
            leaves[symbol] = static_cast<int>(used);
            push(used++);
        }
    while (count > 1) {
        const auto left = pop(), right = pop();
        nodes.at(left).parent = nodes.at(right).parent = static_cast<int>(used);
        nodes.at(used) = {nodes.at(left).weight + nodes.at(right).weight,
                          std::min(nodes.at(left).minimum, nodes.at(right).minimum), -1};
        push(used++);
    }
    std::array<unsigned, 16> lengths{}, next{};
    for (unsigned symbol = 0; symbol < N; ++symbol) {
        auto node = leaves[symbol];
        if (node < 0)
            continue;
        unsigned width = 0;
        while (nodes.at(static_cast<std::size_t>(node)).parent >= 0) {
            if (++width > maximum)
                return false;
            node = nodes.at(static_cast<std::size_t>(node)).parent;
        }
        // Rare skewed alphabets use the universally valid fixed tree instead
        // of a heuristic length-limiting repair that could oversubscribe codes.
        if (!width || width > maximum)
            return false;
        codes[symbol].width = width;
        ++lengths[width];
    }
    unsigned code = 0;
    for (unsigned width = 1; width <= maximum; ++width) {
        code = (code + lengths[width - 1]) << 1;
        next[width] = code;
    }
    for (auto& entry : codes)
        if (entry.width)
            entry.bits = reverse(next[entry.width]++, entry.width);
    return true;
}
struct LengthRun {
    unsigned symbol = 0, value = 0, extra = 0;
};
struct Dynamic {
    std::array<Code, 286> literal{};
    std::array<Code, 30> distance{};
    std::array<Code, 19> lengths{};
    std::array<LengthRun, 316> runs{};
    unsigned literal_count = 286, distance_count = 30, length_count = 19, run_count = 0;
    unsigned header_bits = 0;
};
constexpr std::array<unsigned, 19> length_order{16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                                11, 4,  12, 3, 13, 2, 14, 1, 15};
bool dynamic_header(const std::array<unsigned, 286>& literal_frequency,
                    const std::array<unsigned, 30>& distance_frequency, Dynamic& tree) {
    if (!huffman(literal_frequency, tree.literal, 15) || !huffman(distance_frequency, tree.distance, 15))
        return false;
    while (tree.literal_count > 257 && !tree.literal[tree.literal_count - 1].width)
        --tree.literal_count;
    while (tree.distance_count > 1 && !tree.distance[tree.distance_count - 1].width)
        --tree.distance_count;
    std::array<unsigned, 316> sequence{};
    unsigned count = 0;
    for (unsigned i = 0; i < tree.literal_count; ++i)
        sequence[count++] = tree.literal[i].width;
    for (unsigned i = 0; i < tree.distance_count; ++i)
        sequence[count++] = tree.distance[i].width;
    std::array<unsigned, 19> frequency{};
    const auto add = [&](unsigned symbol, unsigned value = 0, unsigned extra = 0) {
        tree.runs[tree.run_count++] = {symbol, value, extra};
        ++frequency[symbol];
    };
    for (unsigned at = 0; at < count;) {
        const auto value = sequence[at];
        unsigned length = 1;
        while (at + length < count && sequence[at + length] == value)
            ++length;
        at += length;
        if (value) {
            add(value);
            --length;
            while (length >= 3) {
                const auto n = std::min(length, 6u);
                add(16, n - 3, 2);
                length -= n;
            }
        } else {
            while (length >= 3) {
                const bool long_run = length >= 11;
                const auto n = std::min(length, long_run ? 138u : 10u);
                add(long_run ? 18u : 17u, n - (long_run ? 11u : 3u), long_run ? 7u : 3u);
                length -= n;
            }
        }
        while (length) {
            add(value);
            --length;
        }
    }
    if (!huffman(frequency, tree.lengths, 7))
        return false;
    while (tree.length_count > 4 && !tree.lengths[length_order[tree.length_count - 1]].width)
        --tree.length_count;
    tree.header_bits = 5 + 5 + 4 + tree.length_count * 3;
    for (unsigned i = 0; i < tree.run_count; ++i)
        tree.header_bits += tree.lengths[tree.runs[i].symbol].width + tree.runs[i].extra;
    return true;
}
void match(Writer& writer, unsigned length, unsigned distance, const Dynamic* tree) {
    const auto code = length_code[length];
    const auto symbol = tree ? tree->literal[257u + code] : literals[257u + code];
    writer.bits(symbol.bits, symbol.width);
    writer.bits(length - length_base[code], length_extra[code]);
    const auto logarithm = distance > 4 ? static_cast<unsigned>(std::bit_width(distance - 1u) - 1) : 0u;
    const auto dcode = distance_code(distance);
    const auto distance_symbol = tree ? tree->distance[dcode] : Code{reverse(dcode, 5), 5};
    writer.bits(distance_symbol.bits, distance_symbol.width);
    if (distance > 4)
        writer.bits(distance - distance_base[dcode], logarithm - 1u);
}
} // namespace
std::size_t native_deflate(std::string_view input, std::span<char> destination, DeflateWrapper wrapper,
                           const std::function<void()>& check) {
    if (input.size() > 1024 * 1024)
        throw std::invalid_argument("Native bounded compression accepts at most one MiB");
    if (check)
        check();
    Writer writer(destination);
    if (wrapper == DeflateWrapper::gzip) {
        for (const unsigned byte : {0x1fu, 0x8bu, 8u, 0u, 0u, 0u, 0u, 0u, 4u, 255u})
            writer.byte(byte);
    } else {
        writer.byte(0x78);
        writer.byte(0x01); // 32 KiB window, no dictionary, fastest level; FCHECK is valid.
    }
    if (writer.full())
        return 0;
    // musl worker threads can have only a 128 KiB stack. Keep the 192 KiB
    // dictionary/token scratch off that stack, scoped to this one response.
    struct Scratch {
        std::array<std::uint32_t, 16384> positions;
        std::array<std::uint32_t, 32768> tokens;
    };
    auto scratch = std::make_unique_for_overwrite<Scratch>();
    auto& positions = scratch->positions;
    auto& tokens = scratch->tokens;
    positions.fill(std::numeric_limits<std::uint32_t>::max());
    std::size_t offset = 0, next_check = 8192;
    do {
        const auto block_end = std::min(offset + 32768, input.size());
        unsigned token_count = 0;
        std::array<unsigned, 286> literal_frequency{};
        std::array<unsigned, 30> distance_frequency{};
        while (offset + 4 <= block_end) {
            if (offset >= next_check) {
                if (check)
                    check();
                next_check = offset + 8192;
            }
            const auto bucket = hash(input.data() + offset);
            const auto previous = positions[bucket];
            positions[bucket] = static_cast<std::uint32_t>(offset);
            if (previous < offset && offset - previous <= 32768 &&
                word(input.data() + previous) == word(input.data() + offset)) {
                const auto maximum = std::min<std::size_t>(258, block_end - offset);
                std::size_t length = 4;
                while (length + 8 <= maximum) {
                    std::uint64_t a, b;
                    std::memcpy(&a, input.data() + previous + length, sizeof(a));
                    std::memcpy(&b, input.data() + offset + length, sizeof(b));
                    if (a != b)
                        break;
                    length += 8;
                }
                while (length < maximum && input[previous + length] == input[offset + length])
                    ++length;
                const auto distance = static_cast<unsigned>(offset - previous);
                tokens[token_count++] =
                    0x80000000u | ((distance - 1u) << 8) | static_cast<unsigned>(length - 3);
                ++literal_frequency[257u + length_code[length]];
                ++distance_frequency[distance_code(distance)];
                const auto end = offset + length;
                // A bounded dictionary update preserves useful anchors without a
                // hash-chain search or work proportional to repeated matches.
                for (auto at = offset + 1; at + 4 <= end; at += 4)
                    positions[hash(input.data() + at)] = static_cast<std::uint32_t>(at);
                offset = end;
            } else {
                const auto value = static_cast<unsigned char>(input[offset++]);
                tokens[token_count++] = value;
                ++literal_frequency[value];
            }
        }
        while (offset < block_end) {
            const auto value = static_cast<unsigned char>(input[offset++]);
            tokens[token_count++] = value;
            ++literal_frequency[value];
        }
        ++literal_frequency[256];
        // For any binary prefix code H >= log2(total / largest_frequency).
        // Rounding that lower bound down with integer bit_width is conservative:
        // if it alone exceeds the entire destination, no fixed/dynamic tree for
        // these tokens can fit. Reject noisy probes before building three trees.
        const auto symbols = token_count + 1;
        const auto largest = *std::max_element(literal_frequency.begin(), literal_frequency.end());
        if (!largest || largest > symbols)
            throw std::logic_error("Native literal frequency invariant");
        const auto minimum_bits = static_cast<unsigned>(std::bit_width(symbols / largest) - 1);
        const auto minimum_bytes = (static_cast<std::size_t>(symbols) * minimum_bits + 7) / 8;
        if (minimum_bytes > destination.size()) {
            if (check)
                check();
            return 0;
        }
        Dynamic dynamic;
        bool use_dynamic = dynamic_header(literal_frequency, distance_frequency, dynamic);
        if (use_dynamic) {
            unsigned fixed_cost = 0, dynamic_cost = dynamic.header_bits;
            for (unsigned i = 0; i < literal_frequency.size(); ++i) {
                fixed_cost += literal_frequency[i] * literals[i].width;
                dynamic_cost += literal_frequency[i] * dynamic.literal[i].width;
            }
            for (unsigned i = 0; i < distance_frequency.size(); ++i) {
                fixed_cost += distance_frequency[i] * 5;
                dynamic_cost += distance_frequency[i] * dynamic.distance[i].width;
            }
            use_dynamic = dynamic_cost < fixed_cost;
        }
        writer.bits((offset == input.size() ? 1u : 0u) | (use_dynamic ? 4u : 2u), 3);
        if (use_dynamic) {
            writer.bits(dynamic.literal_count - 257, 5);
            writer.bits(dynamic.distance_count - 1, 5);
            writer.bits(dynamic.length_count - 4, 4);
            for (unsigned i = 0; i < dynamic.length_count; ++i)
                writer.bits(dynamic.lengths[length_order[i]].width, 3);
            for (unsigned i = 0; i < dynamic.run_count; ++i) {
                const auto& run = dynamic.runs[i];
                writer.bits(dynamic.lengths[run.symbol].bits, dynamic.lengths[run.symbol].width);
                writer.bits(run.value, run.extra);
            }
        }
        for (unsigned i = 0; i < token_count; ++i) {
            const auto token = tokens[i];
            if (token & 0x80000000u)
                match(writer, (token & 255u) + 3u, ((token >> 8) & 32767u) + 1u,
                      use_dynamic ? &dynamic : nullptr);
            else {
                const auto symbol = use_dynamic ? dynamic.literal[token] : literals[token];
                writer.bits(symbol.bits, symbol.width);
            }
            if (writer.full())
                return 0;
        }
        const auto end = use_dynamic ? dynamic.literal[256] : literals[256];
        writer.bits(end.bits, end.width);
        if (check)
            check();
    } while (offset < input.size());
    writer.align();
    if (writer.full())
        return 0;
    if (check)
        check();
    const auto bytes = reinterpret_cast<const Bytef*>(input.data());
    if (wrapper == DeflateWrapper::gzip) {
        writer.little(static_cast<std::uint32_t>(crc32_z(0, bytes, input.size())));
        writer.little(static_cast<std::uint32_t>(input.size()));
    } else {
        const auto sum = static_cast<std::uint32_t>(adler32_z(1, bytes, input.size()));
        for (int shift = 24; shift >= 0; shift -= 8)
            writer.byte((sum >> shift) & 0xffu);
    }
    if (check)
        check();
    return writer.size();
}
} // namespace devbox
