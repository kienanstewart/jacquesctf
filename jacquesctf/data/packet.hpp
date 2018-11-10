/*
 * Copyright (C) 2018 Philippe Proulx <eepp.ca> - All Rights Reserved
 *
 * Unauthorized copying of this file, via any medium, is strictly
 * prohibited. Proprietary and confidential.
 */

#ifndef _JACQUES_PACKET_HPP
#define _JACQUES_PACKET_HPP

#include <algorithm>
#include <memory>
#include <vector>
#include <yactfr/packet-sequence.hpp>
#include <yactfr/packet-sequence-iterator.hpp>
#include <yactfr/data-source.hpp>
#include <yactfr/metadata/float-type.hpp>
#include <yactfr/metadata/int-type.hpp>
#include <boost/core/noncopyable.hpp>

#include "packet-index-entry.hpp"
#include "packet-checkpoints.hpp"
#include "event-record.hpp"
#include "packet-region.hpp"
#include "packet-segment.hpp"
#include "bit-array.hpp"
#include "content-packet-region.hpp"
#include "packet-checkpoints-build-listener.hpp"
#include "metadata.hpp"
#include "memory-mapped-file.hpp"
#include "lru-cache.hpp"
#include "logging.hpp"

namespace jacques {

/*
 * This object's purpose is to provide packet regions and event records
 * to views. This is the core data required by the packet inspection
 * activity.
 *
 * We keep a cache of packet regions (content packet regions being
 * linked to event records), and a cache of the event records linked in
 * the data region cache (for a faster access by index).
 *
 * Caching is valuable here because of how a typical packet inspection
 * session induces a locality of reference: you're typically going
 * backward or forward from the offset you're inspecting once you find a
 * location of interest, so there will typically be a lot of cache hits.
 *
 * The packet region cache is a sorted vector of contiguous shared
 * packet regions. The caching operation performed by
 * _ensureEventRecordIsCached() makes sure that all the packet regions
 * of at most `_eventRecordCacheMaxSize` event records starting at the
 * requested index minus `_eventRecordCacheMaxSize / 2` are in cache.
 * Substracting `_eventRecordCacheMaxSize / 2` makes packet regions and
 * event records available "around" the requested index, which makes
 * sense for a packet inspection activity because the user is typically
 * inspecting around a given offset.
 *
 * When a packet object is constructed, it caches everything known to be
 * in the preamble segment, that is, everything before the first event
 * record (if any), or all the packet's regions otherwise (including any
 * padding or error packet region before the end of the packet). This
 * preamble packet region cache is kept as a separate cache and copied
 * back to the working cache when we make request an offset located
 * within it. When the working cache is the preamble cache, the event
 * record cache is empty.
 *
 *     preamble             ER 0          ER 1            ER 2
 *     #################----**********----***********-----********----
 *     ^^^^^^^^^^^^^^^^^^^^^
 *     preamble packet region cache
 *
 *     preamble
 *     #########################------------
 *     ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
 *     preamble packet region cache
 *
 *     preamble                 error packet region
 *     #########################!!!!!!!!!!!!!!!!!!!!!
 *     ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
 *     preamble packet region cache
 *
 *     preamble                           error packet region
 *     #########################----------!!!!!!!!!!!
 *     ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
 *     preamble packet region cache
 *
 * If _cacheRegionsFromErsAtCurIt() adds anything to the packet region
 * cache, if the last event record to be part of the cache is the
 * packet's last event record, it also adds any padding or error packet
 * region before the end of the packet:
 *
 *        ER 176   ER 177       ER 294      ER 295         ER 296
 *     ...*******--******----...********----**********-----*******---...
 *                 ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
 *
 *        ER 300   ER 301       ER 487        ER 488
 *     ...*******--******----...**********----***********------------
 *                 ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
 *
 *        ER 300   ER 301       ER 487
 *     ...*******--******----...**********----********!!!!!!!!!!!!!!!
 *                 ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
 *
 * Because elements are naturally sorted in the caches, we can perform a
 * binary search to find a specific element using one of its
 * intrinsically ordered properties (index, offset in packet,
 * timestamp).
 *
 * As of this version, there's a single packet region cache at a given
 * time. To help with the scenario where the user inspects the same
 * distant offsets often, we could create an LRU cache of packet region
 * caches.
 *
 * There's also an LRU cache (offset in packet to packet region) for
 * frequently accessed packet regions by offset (with
 * regionAtOffsetInPacketBits()): when there's a cache miss, the method
 * calls _ensureOffsetInPacketBitsIsCached() to update the packet region
 * and event record caches and then adds the packet region entry to the
 * LRU cache. The LRU cache avoids performing a binary search by
 * _regionCacheItBeforeOrAtOffsetInPacketBits() every time.
 */
class Packet :
    boost::noncopyable
{
public:
    using SP = std::shared_ptr<Packet>;

public:
    explicit Packet(const PacketIndexEntry& indexEntry,
                    yactfr::PacketSequence& seq,
                    const Metadata& metadata,
                    yactfr::DataSource::UP dataSrc,
                    std::unique_ptr<MemoryMappedFile> mmapFile,
                    PacketCheckpointsBuildListener& packetCheckpointsBuildListener);

    template <typename ContainerT>
    void appendRegions(ContainerT& regions,
                       const Index offsetInPacketBits,
                       const Index endOffsetInPacketBits)
    {
        theLogger->debug("Appending packet regions for user in [{} b, {} b[.",
                         offsetInPacketBits, endOffsetInPacketBits);
        assert(offsetInPacketBits < _indexEntry->effectiveTotalSize());
        assert(endOffsetInPacketBits <= _indexEntry->effectiveTotalSize());
        assert(offsetInPacketBits < endOffsetInPacketBits);
        theLogger->debug("Preamble size: {} b.", _preambleSize.bits());

        auto curOffsetInPacketBits = offsetInPacketBits;

        while (true) {
            this->_ensureOffsetInPacketBitsIsCached(curOffsetInPacketBits);

            auto it = this->_regionCacheItBeforeOrAtOffsetInPacketBits(curOffsetInPacketBits);

            /*
             * If `curOffsetInPacketBits` was the exact offset of a packet
             * region, then it is unchanged here.
             */
            curOffsetInPacketBits = (*it)->segment().offsetInPacketBits();
            theLogger->debug("Current offset: {} b.", curOffsetInPacketBits);

            while (true) {
                if (it == std::end(_regionCache)) {
                    // need to cache more
                    theLogger->debug("End of current packet region cache: "
                                     "cache more (current offset: {} b).",
                                     curOffsetInPacketBits);
                    break;
                }

                this->_appendConstRegion(regions, it);
                curOffsetInPacketBits = (*it)->segment().endOffsetInPacketBits();
                ++it;

                if (curOffsetInPacketBits >= endOffsetInPacketBits) {
                    return;
                }
            }
        }
    }

    const PacketRegion& regionAtOffsetInPacketBits(Index offsetInPacketBits);
    const PacketRegion *previousRegion(const PacketRegion& region);
    const PacketRegion& firstRegion();
    const PacketRegion& lastRegion();
    const EventRecord *eventRecordBeforeOrAtNsFromOrigin(long long nsFromOrigin);
    const EventRecord *eventRecordBeforeOrAtCycles(unsigned long long cycles);

    BitArray bitArray(const PacketSegment& segment) const noexcept
    {
        return BitArray {
            _mmapFile->addr() + segment.offsetInPacketBits() / 8,
            segment.offsetInFirstByteBits(),
            segment.size(),
            segment.byteOrder()
        };
    }

    BitArray bitArray(const PacketRegion& region) const noexcept
    {
        return this->bitArray(region.segment());
    }

    BitArray bitArray(const Scope& scope) const noexcept
    {
        return this->bitArray(scope.segment());
    }

    BitArray bitArray(const EventRecord& eventRecord) const noexcept
    {
        return this->bitArray(eventRecord.segment());
    }

    const std::uint8_t *data(const Index offsetInPacketBytes) const
    {
        assert(offsetInPacketBytes < _indexEntry->effectiveTotalSize().bytes());
        return _mmapFile->addr() + offsetInPacketBytes;
    }

    bool hasData() const noexcept
    {
        return _indexEntry->effectiveTotalSize() > 0;
    }

    const PacketIndexEntry& indexEntry() const noexcept
    {
        return *_indexEntry;
    }

    Size eventRecordCount() const noexcept
    {
        return _checkpoints.eventRecordCount();
    }

    const boost::optional<PacketDecodingError>& error() const noexcept
    {
        return _checkpoints.error();
    }

    const EventRecord& eventRecordAtIndexInPacket(const Index reqIndexInPacket)
    {
        assert(reqIndexInPacket < _checkpoints.eventRecordCount());
        this->_ensureEventRecordIsCached(reqIndexInPacket);
        return **this->_eventRecordCacheItFromIndexInPacket(reqIndexInPacket);
    }

    const EventRecord *firstEventRecord() const
    {
        if (_checkpoints.eventRecordCount() == 0) {
            return nullptr;
        }

        return _checkpoints.firstEventRecord().get();
    }

    const EventRecord *lastEventRecord() const
    {
        if (_checkpoints.eventRecordCount() == 0) {
            return nullptr;
        }

        return _checkpoints.lastEventRecord().get();
    }

private:
    using RegionCache = std::vector<PacketRegion::SP>;
    using EventRecordCache = std::vector<EventRecord::SP>;

private:
    /*
     * Caches the whole packet preamble (single time): packet header,
     * packet context, and any padding until the first event record (if
     * any) or any padding/error until the end of the packet.
     *
     * Clears the event record cache.
     */
    void _cachePreambleRegions();

    /*
     * Makes sure that the event record at index `indexInPacket` exists
     * in the caches. If it does not exist, this method caches the
     * requested event record as well as half of
     * `_eventRecordCacheMaxSize` event records around it (if possible),
     * centering the requested event record within its cache.
     */
    void _ensureEventRecordIsCached(Index indexInPacket);

    /*
     * Makes sure that a packet region containing the bit
     * `offsetInPacketBits` exists in cache. If it does not exist, the
     * method finds the closest event record containing this bit and
     * calls _ensureEventRecordIsCached() with its index.
     */
    void _ensureOffsetInPacketBitsIsCached(Index offsetInPacketBits);

    /*
     * Appends all the remaining packet regions starting at the current
     * iterator until any decoding error and then an error packet
     * region.
     */
    void _cacheRegionsAtCurItUntilError();

    /*
     * After clearing the caches, caches all the packet regions from the
     * event records starting at the current iterator, for `erCount`
     * event records.
     */
    void _cacheRegionsFromErsAtCurIt(Index erIndexInPacket, Size erCount);

    /*
     * Appends a single event record (having index `indexInPacket`)
     * worth of packet regions to the cache starting at the current
     * iterator.
     */
    void _cacheRegionsFromOneErAtCurIt(Index indexInPacket);

    /*
     * Appends packet regions to the packet region cache (and updates
     * the event record cache if needed) starting at the current
     * iterator. Stops appending _after_ the iterator's current
     * element's kind is `endElemKind`.
     */
    void _cacheRegionsAtCurIt(yactfr::Element::Kind endElemKind,
                              bool setCurScope, bool setCurEventRecord,
                              Index erIndexInPacket);

    /*
     * Tries to append a padding packet region to the current cache,
     * where this packet region would be located just before the current
     * iterator. This method uses the current cache's last packet region
     * to know if there's padding, and if there is, what should be its
     * byte order. The padding packet region is assigned scope `scope`
     * (can be `nullptr`).
     */
    void _tryCachePaddingRegionBeforeCurIt(Scope::SP scope);

    /*
     * Appends a content packet region to the current cache from the
     * element(s) at the current iterator. Increments the current
     * iterator so that it contains the following element. The content
     * packet region is assigned scope `scope` (cannot be `nullptr`).
     */
    void _cacheContentRegionAtCurIt(Scope::SP scope);

    /*
     * Returns whether or not the packet region cache `cache` contains
     * the bit `offsetInPacketBits`.
     */
    bool _regionCacheContainsOffsetInPacketBits(const RegionCache& cache,
                                                const Index offsetInPacketBits) const
    {
        if (cache.empty()) {
            return false;
        }

        return offsetInPacketBits >= cache.front()->segment().offsetInPacketBits() &&
               offsetInPacketBits < cache.back()->segment().endOffsetInPacketBits();
    }

    /*
     * Returns the packet region, within the current cache, of which the
     * offset is less than or equal to `offsetInPacketBits`.
     */
    RegionCache::const_iterator _regionCacheItBeforeOrAtOffsetInPacketBits(const Index offsetInPacketBits)
    {
        assert(!_regionCache.empty());
        assert(this->_regionCacheContainsOffsetInPacketBits(_regionCache,
                                                            offsetInPacketBits));

        const auto lessThanFunc = [](const auto& offsetInPacketBits,
                                     const auto region) {
            return offsetInPacketBits <
                   region->segment().offsetInPacketBits();
        };
        auto it = std::upper_bound(std::begin(_regionCache),
                                   std::end(_regionCache),
                                   offsetInPacketBits, lessThanFunc);

        assert(it != std::begin(_regionCache));

        // we found one that is greater than, decrement once to find <=
        --it;
        assert((*it)->segment().offsetInPacketBits() <= offsetInPacketBits);
        return it;
    }

    /*
     * Returns the event record cache iterator containing the event
     * record having the index `indexInPacket`.
     */
    EventRecordCache::const_iterator _eventRecordCacheItFromIndexInPacket(const Index indexInPacket)
    {
        assert(!_eventRecordCache.empty());

        auto it = std::begin(_eventRecordCache);

        assert(indexInPacket >= (*it)->indexInPacket() &&
               indexInPacket < (*it)->indexInPacket() + _eventRecordCache.size());
        it += (indexInPacket - (*it)->indexInPacket());
        return it;
    }

    /*
     * Returns whether or not the event record having the index
     * `indexInPacket` exists in the caches.
     */
    bool _eventRecordIsCached(const Index indexInPacket) const
    {
        if (_eventRecordCache.empty()) {
            return false;
        }

        auto& firstEr = *_eventRecordCache.front();
        auto& lastEr = *_eventRecordCache.back();

        return indexInPacket >= firstEr.indexInPacket() &&
               indexInPacket <= lastEr.indexInPacket();
    }

    /*
     * Current iterator's offset (bits) within the packet.
     */
    Index _itOffsetInPacketBits() const noexcept
    {
        return _it.offset() - _indexEntry->offsetInDataStreamBits();
    }

    /*
     * Current iterator's offset (bytes, floored) within the packet.
     */
    Index _itOffsetInPacketBytes() const noexcept
    {
        return this->_itOffsetInPacketBits() / 8;
    }

    /*
     * Creates a content packet region from the current iterator's
     * element known to have type `ElemT`. The content packet region is
     * assigned scope `scope` (cannot be `nullptr`).
     */
    template <typename ElemT>
    ContentPacketRegion::SP _contentRegionFromBitArrayElemAtCurIt(Scope::SP scope)
    {
        auto& elem = static_cast<const ElemT&>(*_it);
        const PacketSegment segment {
            this->_itOffsetInPacketBits(), elem.type().size()
        };

        // okay to move the scope here, it's never used afterwards
        return std::make_shared<ContentPacketRegion>(segment,
                                                     std::move(scope),
                                                     elem.type(),
                                                     ContentPacketRegion::Value {elem.value()});
    }

    void _trySetPreviousRegionOffsetInPacketBits(PacketRegion& region) const
    {
        if (_regionCache.empty()) {
            return;
        }

        region.previousRegionOffsetInPacketBits(_regionCache.back()->segment().offsetInPacketBits());
    }

    template <typename ContainerT, typename IterT>
    void _appendConstRegion(ContainerT& regions, const IterT& it) const
    {
        regions.push_back(std::static_pointer_cast<const PacketRegion>(*it));
    }

    template <typename CpNearestFuncT, typename GetProcFuncT, typename PropT>
    const EventRecord *_eventRecordBeforeOrAtTs(CpNearestFuncT&& cpNearestFunc,
                                                GetProcFuncT&& getProcFuncT,
                                                const PropT prop)
    {
        if (!_metadata->isCorrelatable()) {
            return nullptr;
        }

        if (_checkpoints.eventRecordCount() == 0) {
            return nullptr;
        }

        const auto eventRecord = _checkpoints.lastEventRecord();

        assert(eventRecord);

        if (eventRecord->firstTimestamp() && _indexEntry->endTimestamp() &&
                prop >= std::forward<GetProcFuncT>(getProcFuncT)(*eventRecord->firstTimestamp()) &&
                prop < std::forward<GetProcFuncT>(getProcFuncT)(*_indexEntry->endTimestamp())) {
            // special case: between last event record and end of packet
            return eventRecord.get();
        }

        const auto cp = std::forward<CpNearestFuncT>(cpNearestFunc)(prop);

        if (!cp) {
            return nullptr;
        }

        _it.restorePosition(cp->second);

        auto curIndex = cp->first->indexInPacket();
        auto inEventRecord = false;
        boost::optional<Timestamp> firstTs;
        boost::optional<Index> indexInPacket;

        while (_it != _endIt) {
            switch (_it->kind()) {
            case yactfr::Element::Kind::EVENT_RECORD_BEGINNING:
                inEventRecord = true;
                firstTs = boost::none;
                ++_it;
                break;

            case yactfr::Element::Kind::CLOCK_VALUE:
            {
                if (firstTs || !inEventRecord) {
                    ++_it;
                    break;
                }

                auto& elem = static_cast<const yactfr::ClockValueElement&>(*_it);

                firstTs = Timestamp {elem};

                const auto erProp = std::forward<GetProcFuncT>(getProcFuncT)(*firstTs);

                if (erProp == prop) {
                    indexInPacket = curIndex;
                    _it = _endIt;
                } else if (erProp > prop) {
                    // we're looking for the previous one
                    if (curIndex == 0) {
                        return nullptr;
                    }

                    indexInPacket = curIndex - 1;
                    _it = _endIt;
                } else {
                    ++_it;
                }

                break;
            }

            case yactfr::Element::Kind::EVENT_RECORD_END:
                inEventRecord = false;
                ++curIndex;
                ++_it;
                break;

            default:
                ++_it;
                break;
            }
        }

        if (!indexInPacket || *indexInPacket > _checkpoints.eventRecordCount()) {
            return nullptr;
        }

        this->_ensureEventRecordIsCached(*indexInPacket);
        return _eventRecordCacheItFromIndexInPacket(*indexInPacket)->get();
    }

private:
    const PacketIndexEntry * const _indexEntry;
    const Metadata * const _metadata;
    yactfr::DataSource::UP _dataSrc;
    std::unique_ptr<MemoryMappedFile> _mmapFile;
    yactfr::PacketSequenceIterator _it;
    yactfr::PacketSequenceIterator _endIt;
    PacketCheckpoints _checkpoints;
    RegionCache _regionCache;
    RegionCache _preambleRegionCache;
    EventRecordCache _eventRecordCache;
    LruCache<Index, PacketRegion::SP> _lruRegionCache;
    const Size _eventRecordCacheMaxSize = 500;
    const DataSize _preambleSize;
};

} // namespace jacques

#endif // _JACQUES_PACKET_HPP
