/*
 * Copyright (C) 2018 Philippe Proulx <eepp.ca> - All Rights Reserved
 *
 * Unauthorized copying of this file, via any medium, is strictly
 * prohibited. Proprietary and confidential.
 */

#ifndef _JACQUES_DATA_PKT_CHECKPOINTS_BUILD_LISTENER_HPP
#define _JACQUES_DATA_PKT_CHECKPOINTS_BUILD_LISTENER_HPP

#include <memory>
#include <yactfr/metadata/fwd.hpp>

#include "er.hpp"
#include "pkt-index-entry.hpp"

namespace jacques {

class PktCheckpointsBuildListener
{
protected:
    explicit PktCheckpointsBuildListener() noexcept = default;

public:
    virtual ~PktCheckpointsBuildListener() = default;

    void startBuild(const PktIndexEntry& pktIndexEntry)
    {
        this->_startBuild(pktIndexEntry);
    }

    void update(const Er& er)
    {
        this->_update(er);
    }

    void endBuild()
    {
        this->_endBuild();
    }

protected:
    virtual void _startBuild(const PktIndexEntry& pktIndexEntry);
    virtual void _update(const Er& er);
    virtual void _endBuild();
};

} // namespace jacques

#endif // _JACQUES_DATA_PKT_CHECKPOINTS_BUILD_LISTENER_HPP
