// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace audio_insight {
/**
    Insert-only exact ordered index used by Integrated Loudness gating.

    Values are stored once in high-occupancy B+ tree leaves. Internal nodes
    retain only exact subtree sums and counts, allowing a strict greater-than
    reduction to visit one boundary leaf plus a logarithmic number of aggregate
    ranges. All arenas and scratch storage are reserved by the constructor, so
    insert(), queryGreaterThan(), and clear() do not allocate.

    This class is worker-owned and is not thread-safe.
*/
class IntegratedLoudnessIndex final {
public:
    static constexpr std::size_t leafValueCapacity = 256;
    static constexpr std::size_t internalChildCapacity = 64;
    static constexpr std::size_t leafRebalanceGroupSize = 16;

    struct QueryResult final {
        double sum = 0.0;
        std::uint64_t count = 0;
    };

    struct Statistics final {
        std::uint64_t valueCount = 0;
        std::uint64_t queryCount = 0;
        std::size_t leafNodeCount = 0;
        std::size_t internalNodeCount = 0;
        std::size_t leafNodeCapacity = 0;
        std::size_t internalNodeCapacity = 0;
        std::size_t reservedBytes = 0;
        std::uint32_t treeHeight = 0;
        std::uint32_t lastQueryNodeVisits = 0;
        std::uint32_t lastQueryAggregateReads = 0;
        std::uint32_t lastQueryBoundaryValueReads = 0;
        std::uint32_t maximumQueryNodeVisits = 0;
        std::uint32_t maximumQueryAggregateReads = 0;
        std::uint32_t maximumQueryBoundaryValueReads = 0;
    };

    explicit IntegratedLoudnessIndex(std::size_t maximumValueCount);

    IntegratedLoudnessIndex(const IntegratedLoudnessIndex&) = delete;
    IntegratedLoudnessIndex& operator=(const IntegratedLoudnessIndex&) = delete;

    /** Removes every value while retaining all reserved storage. */
    void clear() noexcept;

    /** Inserts one finite positive value, returning false at the declared capacity. */
    [[nodiscard]] bool insert(double value) noexcept;

    /** Returns the exact count and an aggregate sum for values strictly above threshold. */
    [[nodiscard]] QueryResult queryGreaterThan(double threshold) noexcept;

    [[nodiscard]] Statistics statistics() const noexcept;

private:
    static constexpr std::size_t maximumTreeDepth = 8;
    static constexpr std::uint32_t invalidNodeIndex = UINT32_MAX;

    struct LeafNode final {
        std::array<double, leafValueCapacity> values;
        double sum = 0.0;
        std::uint16_t valueCount = 0;
    };

    struct InternalNode final {
        std::array<std::uint32_t, internalChildCapacity> children;
        double sum = 0.0;
        double maximumValue = 0.0;
        std::uint32_t valueCount = 0;
        std::uint16_t childCount = 0;
        std::uint8_t level = 0;
    };

    struct SearchPath final {
        std::array<std::uint32_t, maximumTreeDepth> nodes { };
        std::array<std::uint16_t, maximumTreeDepth> childPositions { };
        std::size_t depth = 0;
        std::uint32_t leaf = invalidNodeIndex;
    };

    [[nodiscard]] static std::size_t maximumLeafNodeCount(std::size_t maximumValueCount) noexcept;
    [[nodiscard]] static std::size_t maximumInternalNodeCount(
        std::size_t maximumLeafCount) noexcept;
    [[nodiscard]] bool appendLeaf(std::uint32_t& index) noexcept;
    [[nodiscard]] bool appendInternal(std::uint8_t level, std::uint32_t& index) noexcept;
    [[nodiscard]] bool findLeaf(double value, SearchPath& path) const noexcept;
    [[nodiscard]] double childMaximumValue(
        std::uint8_t childLevel, std::uint32_t childIndex) const noexcept;
    [[nodiscard]] double childSum(std::uint8_t childLevel, std::uint32_t childIndex) const noexcept;
    [[nodiscard]] std::uint32_t childValueCount(
        std::uint8_t childLevel, std::uint32_t childIndex) const noexcept;
    void insertIntoLeaf(LeafNode& leaf, double value) noexcept;
    void recomputeLeaf(LeafNode& leaf) noexcept;
    void recomputeInternal(InternalNode& node) noexcept;
    void refreshAncestors(const SearchPath& path, std::size_t depth) noexcept;
    [[nodiscard]] bool splitRootLeaf(double value) noexcept;
    [[nodiscard]] bool rebalanceLeafGroup(const SearchPath& path, double value) noexcept;
    void refillLeafGroup(const std::array<std::uint32_t, leafRebalanceGroupSize + 1>& leaves,
        std::size_t leafCount, std::size_t valueCount) noexcept;
    [[nodiscard]] bool insertChildAndPropagate(const SearchPath& path, std::size_t pathIndex,
        std::size_t insertionPosition, std::uint32_t newChild) noexcept;
    void resetQueryStatistics() noexcept;
    void finishQueryStatistics() noexcept;

    const std::size_t maximumValueCount_;
    std::vector<LeafNode> leaves_;
    std::vector<InternalNode> internalNodes_;
    std::array<double, (leafRebalanceGroupSize * leafValueCapacity) + 1> leafScratch_ { };
    std::array<std::uint32_t, internalChildCapacity + 1> childScratch_ { };
    std::uint64_t valueCount_ = 0;
    std::uint64_t queryCount_ = 0;
    std::uint32_t rootIndex_ = invalidNodeIndex;
    std::uint8_t rootLevel_ = 0;
    std::uint32_t lastQueryNodeVisits_ = 0;
    std::uint32_t lastQueryAggregateReads_ = 0;
    std::uint32_t lastQueryBoundaryValueReads_ = 0;
    std::uint32_t maximumQueryNodeVisits_ = 0;
    std::uint32_t maximumQueryAggregateReads_ = 0;
    std::uint32_t maximumQueryBoundaryValueReads_ = 0;
};
} // namespace audio_insight
