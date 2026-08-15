// SPDX-License-Identifier: AGPL-3.0-or-later

#include "IntegratedLoudnessIndex.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace audio_insight {
namespace {
constexpr auto minimumStableLeafValues
    = (IntegratedLoudnessIndex::leafRebalanceGroupSize * IntegratedLoudnessIndex::leafValueCapacity
          + 1)
    / (IntegratedLoudnessIndex::leafRebalanceGroupSize + 1);
constexpr auto minimumInternalChildren = IntegratedLoudnessIndex::internalChildCapacity / 2;

[[nodiscard]] constexpr std::size_t divideRoundedUp(
    const std::size_t numerator, const std::size_t denominator) noexcept
{
    return numerator == 0 ? 0 : 1 + ((numerator - 1) / denominator);
}
} // namespace

IntegratedLoudnessIndex::IntegratedLoudnessIndex(const std::size_t maximumValueCount)
    : maximumValueCount_(maximumValueCount)
{
    const auto leafCapacity = maximumLeafNodeCount(maximumValueCount_);
    leaves_.reserve(leafCapacity);
    internalNodes_.reserve(maximumInternalNodeCount(leafCapacity));
    clear();
}

void IntegratedLoudnessIndex::clear() noexcept
{
    leaves_.clear();
    internalNodes_.clear();
    valueCount_ = 0;
    queryCount_ = 0;
    rootIndex_ = invalidNodeIndex;
    rootLevel_ = 0;
    lastQueryNodeVisits_ = 0;
    lastQueryAggregateReads_ = 0;
    lastQueryBoundaryValueReads_ = 0;
    maximumQueryNodeVisits_ = 0;
    maximumQueryAggregateReads_ = 0;
    maximumQueryBoundaryValueReads_ = 0;

    const auto appended = appendLeaf(rootIndex_);
    static_cast<void>(appended);
}

bool IntegratedLoudnessIndex::insert(const double value) noexcept
{
    if (!std::isfinite(value) || value <= 0.0
        || valueCount_ >= static_cast<std::uint64_t>(maximumValueCount_)) {
        return false;
    }

    SearchPath path;
    if (!findLeaf(value, path))
        return false;

    auto& leaf = leaves_[path.leaf];
    if (leaf.valueCount < leafValueCapacity) {
        insertIntoLeaf(leaf, value);
        ++valueCount_;
        refreshAncestors(path, path.depth);
        return true;
    }

    const auto inserted = path.depth == 0 ? splitRootLeaf(value) : rebalanceLeafGroup(path, value);
    if (inserted)
        ++valueCount_;
    return inserted;
}

IntegratedLoudnessIndex::QueryResult IntegratedLoudnessIndex::queryGreaterThan(
    const double threshold) noexcept
{
    resetQueryStatistics();
    ++queryCount_;

    QueryResult result;
    if (valueCount_ == 0 || std::isnan(threshold)) {
        finishQueryStatistics();
        return result;
    }

    SearchPath path;
    auto level = rootLevel_;
    auto nodeIndex = rootIndex_;
    while (level != 0) {
        if (path.depth >= maximumTreeDepth) {
            finishQueryStatistics();
            return { };
        }

        ++lastQueryNodeVisits_;
        const auto& node = internalNodes_[nodeIndex];
        auto first = std::size_t { 0 };
        auto last = static_cast<std::size_t>(node.childCount);
        while (first < last) {
            const auto middle = first + ((last - first) / 2);
            if (childMaximumValue(level - 1, node.children[middle]) > threshold)
                last = middle;
            else
                first = middle + 1;
        }

        if (first == node.childCount) {
            finishQueryStatistics();
            return result;
        }

        path.nodes[path.depth] = nodeIndex;
        path.childPositions[path.depth] = static_cast<std::uint16_t>(first);
        ++path.depth;
        nodeIndex = node.children[first];
        --level;
    }

    ++lastQueryNodeVisits_;
    const auto& leaf = leaves_[nodeIndex];
    const auto firstValue
        = std::upper_bound(leaf.values.begin(), leaf.values.begin() + leaf.valueCount, threshold);
    for (auto value = firstValue; value != leaf.values.begin() + leaf.valueCount; ++value) {
        result.sum += *value;
        ++result.count;
        ++lastQueryBoundaryValueReads_;
    }

    for (auto depth = path.depth; depth != 0; --depth) {
        const auto& node = internalNodes_[path.nodes[depth - 1]];
        const auto firstSibling = static_cast<std::size_t>(path.childPositions[depth - 1]) + 1;
        for (auto child = firstSibling; child < node.childCount; ++child) {
            result.sum += childSum(node.level - 1, node.children[child]);
            result.count += childValueCount(node.level - 1, node.children[child]);
            ++lastQueryAggregateReads_;
        }
    }

    finishQueryStatistics();
    return result;
}

IntegratedLoudnessIndex::Statistics IntegratedLoudnessIndex::statistics() const noexcept
{
    Statistics result;
    result.valueCount = valueCount_;
    result.queryCount = queryCount_;
    result.leafNodeCount = leaves_.size();
    result.internalNodeCount = internalNodes_.size();
    result.leafNodeCapacity = leaves_.capacity();
    result.internalNodeCapacity = internalNodes_.capacity();
    result.reservedBytes = sizeof(*this) + (leaves_.capacity() * sizeof(LeafNode))
        + (internalNodes_.capacity() * sizeof(InternalNode));
    result.treeHeight = rootIndex_ == invalidNodeIndex ? 0 : rootLevel_ + 1;
    result.lastQueryNodeVisits = lastQueryNodeVisits_;
    result.lastQueryAggregateReads = lastQueryAggregateReads_;
    result.lastQueryBoundaryValueReads = lastQueryBoundaryValueReads_;
    result.maximumQueryNodeVisits = maximumQueryNodeVisits_;
    result.maximumQueryAggregateReads = maximumQueryAggregateReads_;
    result.maximumQueryBoundaryValueReads = maximumQueryBoundaryValueReads_;
    return result;
}

std::size_t IntegratedLoudnessIndex::maximumLeafNodeCount(
    const std::size_t maximumValueCount) noexcept
{
    // Before the first internal-root split, at most one root fanout of leaves
    // may retain the lower occupancy created by the initial two-way split.
    // Every later leaf is created only after a full 16-leaf group is split
    // into 17 leaves, leaving at least 241 exact values in each leaf.
    return internalChildCapacity + divideRoundedUp(maximumValueCount, minimumStableLeafValues) + 2;
}

std::size_t IntegratedLoudnessIndex::maximumInternalNodeCount(
    const std::size_t maximumLeafCount) noexcept
{
    // Every non-root internal node has at least 32 children. The additional
    // depth allowance covers the successively replaced roots conservatively.
    return divideRoundedUp(maximumLeafCount, minimumInternalChildren - 1) + maximumTreeDepth + 2;
}

bool IntegratedLoudnessIndex::appendLeaf(std::uint32_t& index) noexcept
{
    if (leaves_.size() >= leaves_.capacity()
        || leaves_.size() >= static_cast<std::size_t>(invalidNodeIndex)) {
        return false;
    }

    index = static_cast<std::uint32_t>(leaves_.size());
    leaves_.emplace_back();
    return true;
}

bool IntegratedLoudnessIndex::appendInternal(
    const std::uint8_t level, std::uint32_t& index) noexcept
{
    if (internalNodes_.size() >= internalNodes_.capacity()
        || internalNodes_.size() >= static_cast<std::size_t>(invalidNodeIndex)) {
        return false;
    }

    index = static_cast<std::uint32_t>(internalNodes_.size());
    internalNodes_.emplace_back();
    internalNodes_.back().level = level;
    return true;
}

bool IntegratedLoudnessIndex::findLeaf(const double value, SearchPath& path) const noexcept
{
    if (rootIndex_ == invalidNodeIndex)
        return false;

    auto level = rootLevel_;
    auto nodeIndex = rootIndex_;
    while (level != 0) {
        if (path.depth >= maximumTreeDepth)
            return false;

        const auto& node = internalNodes_[nodeIndex];
        if (node.childCount == 0)
            return false;

        auto first = std::size_t { 0 };
        auto last = static_cast<std::size_t>(node.childCount);
        while (first < last) {
            const auto middle = first + ((last - first) / 2);
            if (value <= childMaximumValue(level - 1, node.children[middle]))
                last = middle;
            else
                first = middle + 1;
        }

        const auto position = std::min(first, static_cast<std::size_t>(node.childCount - 1));
        path.nodes[path.depth] = nodeIndex;
        path.childPositions[path.depth] = static_cast<std::uint16_t>(position);
        ++path.depth;
        nodeIndex = node.children[position];
        --level;
    }

    path.leaf = nodeIndex;
    return path.leaf < leaves_.size();
}

double IntegratedLoudnessIndex::childMaximumValue(
    const std::uint8_t childLevel, const std::uint32_t childIndex) const noexcept
{
    if (childLevel == 0) {
        const auto& leaf = leaves_[childIndex];
        return leaf.valueCount == 0 ? -std::numeric_limits<double>::infinity()
                                    : leaf.values[leaf.valueCount - 1];
    }
    return internalNodes_[childIndex].maximumValue;
}

double IntegratedLoudnessIndex::childSum(
    const std::uint8_t childLevel, const std::uint32_t childIndex) const noexcept
{
    return childLevel == 0 ? leaves_[childIndex].sum : internalNodes_[childIndex].sum;
}

std::uint32_t IntegratedLoudnessIndex::childValueCount(
    const std::uint8_t childLevel, const std::uint32_t childIndex) const noexcept
{
    return childLevel == 0 ? leaves_[childIndex].valueCount : internalNodes_[childIndex].valueCount;
}

void IntegratedLoudnessIndex::insertIntoLeaf(LeafNode& leaf, const double value) noexcept
{
    const auto end = leaf.values.begin() + leaf.valueCount;
    const auto position = std::upper_bound(leaf.values.begin(), end, value);
    std::move_backward(position, end, end + 1);
    *position = value;
    ++leaf.valueCount;
    leaf.sum += value;
}

void IntegratedLoudnessIndex::recomputeLeaf(LeafNode& leaf) noexcept
{
    leaf.sum = 0.0;
    for (auto index = std::size_t { 0 }; index < leaf.valueCount; ++index)
        leaf.sum += leaf.values[index];
}

void IntegratedLoudnessIndex::recomputeInternal(InternalNode& node) noexcept
{
    node.sum = 0.0;
    node.valueCount = 0;
    for (auto child = std::size_t { 0 }; child < node.childCount; ++child) {
        node.sum += childSum(node.level - 1, node.children[child]);
        node.valueCount += childValueCount(node.level - 1, node.children[child]);
    }
    node.maximumValue = node.childCount == 0
        ? -std::numeric_limits<double>::infinity()
        : childMaximumValue(node.level - 1, node.children[node.childCount - 1]);
}

void IntegratedLoudnessIndex::refreshAncestors(
    const SearchPath& path, const std::size_t depth) noexcept
{
    for (auto remaining = depth; remaining != 0; --remaining)
        recomputeInternal(internalNodes_[path.nodes[remaining - 1]]);
}

bool IntegratedLoudnessIndex::splitRootLeaf(const double value) noexcept
{
    if (leaves_.size() + 1 > leaves_.capacity()
        || internalNodes_.size() + 1 > internalNodes_.capacity()) {
        return false;
    }

    const auto oldRoot = rootIndex_;
    const auto& rootLeaf = leaves_[oldRoot];
    std::copy(rootLeaf.values.begin(), rootLeaf.values.begin() + rootLeaf.valueCount,
        leafScratch_.begin());
    auto insertion
        = std::upper_bound(leafScratch_.begin(), leafScratch_.begin() + rootLeaf.valueCount, value);
    std::move_backward(insertion, leafScratch_.begin() + rootLeaf.valueCount,
        leafScratch_.begin() + rootLeaf.valueCount + 1);
    *insertion = value;

    std::uint32_t newLeaf = invalidNodeIndex;
    if (!appendLeaf(newLeaf))
        return false;

    std::array<std::uint32_t, leafRebalanceGroupSize + 1> leaves { };
    leaves[0] = oldRoot;
    leaves[1] = newLeaf;
    refillLeafGroup(leaves, 2, leafValueCapacity + 1);

    std::uint32_t newRoot = invalidNodeIndex;
    if (!appendInternal(1, newRoot))
        return false;
    auto& root = internalNodes_[newRoot];
    root.children[0] = oldRoot;
    root.children[1] = newLeaf;
    root.childCount = 2;
    recomputeInternal(root);
    rootIndex_ = newRoot;
    rootLevel_ = 1;
    return true;
}

bool IntegratedLoudnessIndex::rebalanceLeafGroup(
    const SearchPath& path, const double value) noexcept
{
    const auto pathIndex = path.depth - 1;
    const auto parentIndex = path.nodes[pathIndex];
    const auto& parent = internalNodes_[parentIndex];
    const auto groupCount = std::min<std::size_t>(parent.childCount, leafRebalanceGroupSize);
    const auto targetPosition = static_cast<std::size_t>(path.childPositions[pathIndex]);
    auto groupStart = targetPosition > groupCount / 2 ? targetPosition - (groupCount / 2) : 0;
    groupStart = std::min(groupStart, static_cast<std::size_t>(parent.childCount) - groupCount);

    std::array<std::uint32_t, leafRebalanceGroupSize + 1> groupLeaves { };
    auto scratchValueCount = std::size_t { 0 };
    for (auto groupOffset = std::size_t { 0 }; groupOffset < groupCount; ++groupOffset) {
        const auto leafIndex = parent.children[groupStart + groupOffset];
        groupLeaves[groupOffset] = leafIndex;
        const auto& leaf = leaves_[leafIndex];
        std::copy(leaf.values.begin(), leaf.values.begin() + leaf.valueCount,
            leafScratch_.begin() + scratchValueCount);
        scratchValueCount += leaf.valueCount;
    }

    auto insertion
        = std::upper_bound(leafScratch_.begin(), leafScratch_.begin() + scratchValueCount, value);
    std::move_backward(insertion, leafScratch_.begin() + scratchValueCount,
        leafScratch_.begin() + scratchValueCount + 1);
    *insertion = value;
    ++scratchValueCount;

    const auto needsNewLeaf = scratchValueCount > groupCount * leafValueCapacity;
    if (!needsNewLeaf) {
        refillLeafGroup(groupLeaves, groupCount, scratchValueCount);
        refreshAncestors(path, path.depth);
        return true;
    }

    if (leaves_.size() + 1 > leaves_.capacity()
        || internalNodes_.size() + path.depth + 1 > internalNodes_.capacity()) {
        return false;
    }

    std::uint32_t newLeaf = invalidNodeIndex;
    if (!appendLeaf(newLeaf))
        return false;
    groupLeaves[groupCount] = newLeaf;
    refillLeafGroup(groupLeaves, groupCount + 1, scratchValueCount);
    return insertChildAndPropagate(path, pathIndex, groupStart + groupCount, newLeaf);
}

void IntegratedLoudnessIndex::refillLeafGroup(
    const std::array<std::uint32_t, leafRebalanceGroupSize + 1>& leaves,
    const std::size_t leafCount, const std::size_t valueCount) noexcept
{
    const auto baseValueCount = valueCount / leafCount;
    const auto extraLeafCount = valueCount % leafCount;
    auto sourceOffset = std::size_t { 0 };
    for (auto leafOffset = std::size_t { 0 }; leafOffset < leafCount; ++leafOffset) {
        auto& leaf = leaves_[leaves[leafOffset]];
        const auto destinationCount = baseValueCount + (leafOffset < extraLeafCount ? 1 : 0);
        std::copy(leafScratch_.begin() + sourceOffset,
            leafScratch_.begin() + sourceOffset + destinationCount, leaf.values.begin());
        leaf.valueCount = static_cast<std::uint16_t>(destinationCount);
        recomputeLeaf(leaf);
        sourceOffset += destinationCount;
    }
}

bool IntegratedLoudnessIndex::insertChildAndPropagate(const SearchPath& path, std::size_t pathIndex,
    std::size_t insertionPosition, std::uint32_t newChild) noexcept
{
    while (true) {
        const auto nodeIndex = path.nodes[pathIndex];
        auto& node = internalNodes_[nodeIndex];
        if (node.childCount < internalChildCapacity) {
            auto insertion = node.children.begin() + insertionPosition;
            std::move_backward(insertion, node.children.begin() + node.childCount,
                node.children.begin() + node.childCount + 1);
            *insertion = newChild;
            ++node.childCount;
            recomputeInternal(node);
            refreshAncestors(path, pathIndex);
            return true;
        }

        std::copy(
            node.children.begin(), node.children.begin() + node.childCount, childScratch_.begin());
        auto insertion = childScratch_.begin() + insertionPosition;
        std::move_backward(insertion, childScratch_.begin() + node.childCount,
            childScratch_.begin() + node.childCount + 1);
        *insertion = newChild;

        constexpr auto splitChildCount = internalChildCapacity + 1;
        constexpr auto leftChildCount = splitChildCount / 2;
        constexpr auto rightChildCount = splitChildCount - leftChildCount;
        std::copy(
            childScratch_.begin(), childScratch_.begin() + leftChildCount, node.children.begin());
        node.childCount = static_cast<std::uint16_t>(leftChildCount);
        recomputeInternal(node);

        std::uint32_t siblingIndex = invalidNodeIndex;
        if (!appendInternal(node.level, siblingIndex))
            return false;
        auto& sibling = internalNodes_[siblingIndex];
        std::copy(childScratch_.begin() + leftChildCount, childScratch_.begin() + splitChildCount,
            sibling.children.begin());
        sibling.childCount = static_cast<std::uint16_t>(rightChildCount);
        recomputeInternal(sibling);

        if (pathIndex == 0) {
            std::uint32_t newRoot = invalidNodeIndex;
            if (!appendInternal(node.level + 1, newRoot))
                return false;
            auto& root = internalNodes_[newRoot];
            root.children[0] = nodeIndex;
            root.children[1] = siblingIndex;
            root.childCount = 2;
            recomputeInternal(root);
            rootIndex_ = newRoot;
            ++rootLevel_;
            return true;
        }

        newChild = siblingIndex;
        insertionPosition = static_cast<std::size_t>(path.childPositions[pathIndex - 1]) + 1;
        --pathIndex;
    }
}

void IntegratedLoudnessIndex::resetQueryStatistics() noexcept
{
    lastQueryNodeVisits_ = 0;
    lastQueryAggregateReads_ = 0;
    lastQueryBoundaryValueReads_ = 0;
}

void IntegratedLoudnessIndex::finishQueryStatistics() noexcept
{
    maximumQueryNodeVisits_ = std::max(maximumQueryNodeVisits_, lastQueryNodeVisits_);
    maximumQueryAggregateReads_ = std::max(maximumQueryAggregateReads_, lastQueryAggregateReads_);
    maximumQueryBoundaryValueReads_
        = std::max(maximumQueryBoundaryValueReads_, lastQueryBoundaryValueReads_);
}
} // namespace audio_insight
