/*
 * Copyright (C) 2018 Philippe Proulx <eepp.ca> - All Rights Reserved
 *
 * Unauthorized copying of this file, via any medium, is strictly
 * prohibited. Proprietary and confidential.
 */

#include <cassert>

#include "packet-state.hpp"
#include "packet-region.hpp"
#include "state.hpp"
#include "cur-offset-in-packet-changed-message.hpp"

namespace jacques {

PacketState::PacketState(State& state, Packet& packet) :
    _state {&state},
    _packet {&packet}
{
}

void PacketState::gotoPreviousEventRecord(Size count)
{
    if (_packet->eventRecordCount() == 0) {
        return;
    }

    const auto curEventRecord = this->currentEventRecord();

    if (!curEventRecord) {
        if (_curOffsetInPacketBits >=
                _packet->indexEntry().effectiveContentSize().bits()) {
            auto lastEr = _packet->lastEventRecord();

            assert(lastEr);
            this->gotoPacketRegionAtOffsetInPacketBits(lastEr->segment().offsetInPacketBits());
        }

        return;
    }

    if (curEventRecord->indexInPacket() == 0) {
        return;
    }

    count = std::min(curEventRecord->indexInPacket(), count);

    const auto& prevEventRecord = _packet->eventRecordAtIndexInPacket(curEventRecord->indexInPacket() - count);

    this->gotoPacketRegionAtOffsetInPacketBits(prevEventRecord.segment().offsetInPacketBits());
}

void PacketState::gotoNextEventRecord(Size count)
{
    if (_packet->eventRecordCount() == 0) {
        return;
    }

    const auto curEventRecord = this->currentEventRecord();
    Index newIndex = 0;

    if (curEventRecord) {
        count = std::min(_packet->eventRecordCount() -
                         curEventRecord->indexInPacket(), count);
        newIndex = curEventRecord->indexInPacket() + count;
    }

    if (newIndex >= _packet->eventRecordCount()) {
        return;
    }

    const auto& nextEventRecord = _packet->eventRecordAtIndexInPacket(newIndex);

    this->gotoPacketRegionAtOffsetInPacketBits(nextEventRecord.segment().offsetInPacketBits());
}

void PacketState::gotoPreviousPacketRegion()
{
    if (!_packet->hasData()) {
        return;
    }

    if (_curOffsetInPacketBits == 0) {
        return;
    }

    const auto& currentPacketRegion = this->currentPacketRegion();

    if (currentPacketRegion.previousPacketRegionOffsetInPacketBits()) {
        this->gotoPacketRegionAtOffsetInPacketBits(*currentPacketRegion.previousPacketRegionOffsetInPacketBits());
        return;
    }

    const auto& prevPacketRegion = _packet->packetRegionAtOffsetInPacketBits(_curOffsetInPacketBits - 1);

    this->gotoPacketRegionAtOffsetInPacketBits(prevPacketRegion);
}

void PacketState::gotoNextPacketRegion()
{
    if (!_packet->hasData()) {
        return;
    }

    const auto& currentPacketRegion = this->currentPacketRegion();

    if (currentPacketRegion.segment().endOffsetInPacketBits() ==
            _packet->indexEntry().effectiveTotalSize().bits()) {
        return;
    }

    this->gotoPacketRegionAtOffsetInPacketBits(currentPacketRegion.segment().endOffsetInPacketBits());
}

void PacketState::gotoPacketContext()
{
    const auto& offset = _packet->indexEntry().packetContextOffsetInPacketBits();

    if (!offset) {
        return;
    }

    this->gotoPacketRegionAtOffsetInPacketBits(*offset);
}

void PacketState::gotoLastPacketRegion()
{
    this->gotoPacketRegionAtOffsetInPacketBits(_packet->lastPacketRegion());
}

void PacketState::gotoPacketRegionAtOffsetInPacketBits(const Index offsetInPacketBits)
{
    if (offsetInPacketBits == _curOffsetInPacketBits) {
        return;
    }

    assert(offsetInPacketBits < _packet->indexEntry().effectiveTotalSize().bits());
    _curOffsetInPacketBits = offsetInPacketBits;
    _state->_notify(CurOffsetInPacketChangedMessage {});
}


} // namespace jacques