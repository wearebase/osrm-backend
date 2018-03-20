#ifndef SHARED_DATA_TYPE_HPP
#define SHARED_DATA_TYPE_HPP

#include "storage/block.hpp"

#include "util/exception.hpp"
#include "util/exception_utils.hpp"
#include "util/log.hpp"

#include <boost/assert.hpp>

#include <array>
#include <cstdint>

namespace osrm
{
namespace storage
{

// Added at the start and end of each block as sanity check
const constexpr char CANARY[4] = {'O', 'S', 'R', 'M'};

const constexpr char *block_id_to_name[] = {"NAME_CHAR_DATA",
                                            "EDGE_BASED_NODE_DATA",
                                            "ANNOTATION_DATA",
                                            "CH_GRAPH_NODE_LIST",
                                            "CH_GRAPH_EDGE_LIST",
                                            "CH_EDGE_FILTER_0",
                                            "CH_EDGE_FILTER_1",
                                            "CH_EDGE_FILTER_2",
                                            "CH_EDGE_FILTER_3",
                                            "CH_EDGE_FILTER_4",
                                            "CH_EDGE_FILTER_5",
                                            "CH_EDGE_FILTER_6",
                                            "CH_EDGE_FILTER_7",
                                            "COORDINATE_LIST",
                                            "OSM_NODE_ID_LIST",
                                            "TURN_INSTRUCTION",
                                            "ENTRY_CLASSID",
                                            "R_SEARCH_TREE",
                                            "R_SEARCH_TREE_LEVELS",
                                            "GEOMETRIES_INDEX",
                                            "GEOMETRIES_NODE_LIST",
                                            "GEOMETRIES_FWD_WEIGHT_LIST",
                                            "GEOMETRIES_REV_WEIGHT_LIST",
                                            "GEOMETRIES_FWD_DURATION_LIST",
                                            "GEOMETRIES_REV_DURATION_LIST",
                                            "GEOMETRIES_FWD_DATASOURCES_LIST",
                                            "GEOMETRIES_REV_DATASOURCES_LIST",
                                            "HSGR_CHECKSUM",
                                            "TIMESTAMP",
                                            "FILE_INDEX_PATH",
                                            "DATASOURCES_NAMES",
                                            "PROPERTIES",
                                            "BEARING_CLASSID",
                                            "BEARING_OFFSETS",
                                            "BEARING_BLOCKS",
                                            "BEARING_VALUES",
                                            "ENTRY_CLASS",
                                            "LANE_DATA_ID",
                                            "PRE_TURN_BEARING",
                                            "POST_TURN_BEARING",
                                            "TURN_LANE_DATA",
                                            "LANE_DESCRIPTION_OFFSETS",
                                            "LANE_DESCRIPTION_MASKS",
                                            "TURN_WEIGHT_PENALTIES",
                                            "TURN_DURATION_PENALTIES",
                                            "MLD_LEVEL_DATA",
                                            "MLD_PARTITION",
                                            "MLD_CELL_TO_CHILDREN",
                                            "MLD_CELL_WEIGHTS_0",
                                            "MLD_CELL_WEIGHTS_1",
                                            "MLD_CELL_WEIGHTS_2",
                                            "MLD_CELL_WEIGHTS_3",
                                            "MLD_CELL_WEIGHTS_4",
                                            "MLD_CELL_WEIGHTS_5",
                                            "MLD_CELL_WEIGHTS_6",
                                            "MLD_CELL_WEIGHTS_7",
                                            "MLD_CELL_DURATIONS_0",
                                            "MLD_CELL_DURATIONS_1",
                                            "MLD_CELL_DURATIONS_2",
                                            "MLD_CELL_DURATIONS_3",
                                            "MLD_CELL_DURATIONS_4",
                                            "MLD_CELL_DURATIONS_5",
                                            "MLD_CELL_DURATIONS_6",
                                            "MLD_CELL_DURATIONS_7",
                                            "MLD_CELL_SOURCE_BOUNDARY",
                                            "MLD_CELL_DESTINATION_BOUNDARY",
                                            "MLD_CELLS",
                                            "MLD_CELL_LEVEL_OFFSETS",
                                            "MLD_GRAPH_NODE_LIST",
                                            "MLD_GRAPH_EDGE_LIST",
                                            "MLD_GRAPH_NODE_TO_OFFSET",
                                            "MANEUVER_OVERRIDES",
                                            "MANEUVER_OVERRIDE_NODE_SEQUENCES"};

struct DataLayout
{
    enum BlockID
    {
        NAME_CHAR_DATA = 0,
        EDGE_BASED_NODE_DATA_LIST,
        ANNOTATION_DATA_LIST,
        CH_GRAPH_NODE_LIST,
        CH_GRAPH_EDGE_LIST,
        CH_EDGE_FILTER_0,
        CH_EDGE_FILTER_1,
        CH_EDGE_FILTER_2,
        CH_EDGE_FILTER_3,
        CH_EDGE_FILTER_4,
        CH_EDGE_FILTER_5,
        CH_EDGE_FILTER_6,
        CH_EDGE_FILTER_7,
        COORDINATE_LIST,
        OSM_NODE_ID_LIST,
        TURN_INSTRUCTION,
        ENTRY_CLASSID,
        R_SEARCH_TREE,
        R_SEARCH_TREE_LEVELS,
        GEOMETRIES_INDEX,
        GEOMETRIES_NODE_LIST,
        GEOMETRIES_FWD_WEIGHT_LIST,
        GEOMETRIES_REV_WEIGHT_LIST,
        GEOMETRIES_FWD_DURATION_LIST,
        GEOMETRIES_REV_DURATION_LIST,
        GEOMETRIES_FWD_DATASOURCES_LIST,
        GEOMETRIES_REV_DATASOURCES_LIST,
        HSGR_CHECKSUM,
        TIMESTAMP,
        FILE_INDEX_PATH,
        DATASOURCES_NAMES,
        PROPERTIES,
        BEARING_CLASSID,
        BEARING_OFFSETS,
        BEARING_BLOCKS,
        BEARING_VALUES,
        ENTRY_CLASS,
        LANE_DATA_ID,
        PRE_TURN_BEARING,
        POST_TURN_BEARING,
        TURN_LANE_DATA,
        LANE_DESCRIPTION_OFFSETS,
        LANE_DESCRIPTION_MASKS,
        TURN_WEIGHT_PENALTIES,
        TURN_DURATION_PENALTIES,
        MLD_LEVEL_DATA,
        MLD_PARTITION,
        MLD_CELL_TO_CHILDREN,
        MLD_CELL_WEIGHTS_0,
        MLD_CELL_WEIGHTS_1,
        MLD_CELL_WEIGHTS_2,
        MLD_CELL_WEIGHTS_3,
        MLD_CELL_WEIGHTS_4,
        MLD_CELL_WEIGHTS_5,
        MLD_CELL_WEIGHTS_6,
        MLD_CELL_WEIGHTS_7,
        MLD_CELL_DURATIONS_0,
        MLD_CELL_DURATIONS_1,
        MLD_CELL_DURATIONS_2,
        MLD_CELL_DURATIONS_3,
        MLD_CELL_DURATIONS_4,
        MLD_CELL_DURATIONS_5,
        MLD_CELL_DURATIONS_6,
        MLD_CELL_DURATIONS_7,
        MLD_CELL_SOURCE_BOUNDARY,
        MLD_CELL_DESTINATION_BOUNDARY,
        MLD_CELLS,
        MLD_CELL_LEVEL_OFFSETS,
        MLD_GRAPH_NODE_LIST,
        MLD_GRAPH_EDGE_LIST,
        MLD_GRAPH_NODE_TO_OFFSET,
        MANEUVER_OVERRIDES,
        MANEUVER_OVERRIDE_NODE_SEQUENCES,
        NUM_BLOCKS
    };

    std::array<Block, NUM_BLOCKS> blocks;

    DataLayout() : blocks{} {}

    inline void SetBlock(BlockID bid, Block block) { blocks[bid] = std::move(block); }

    inline uint64_t GetBlockEntries(BlockID bid) const { return blocks[bid].num_entries; }

    inline uint64_t GetBlockSize(BlockID bid) const { return blocks[bid].byte_size; }

    inline uint64_t GetSizeOfLayout() const
    {
        uint64_t result = 0;
        for (auto i = 0; i < NUM_BLOCKS; i++)
        {
            BOOST_ASSERT(blocks[i].entry_align > 0);
            result += 2 * sizeof(CANARY) + GetBlockSize((BlockID)i) + blocks[i].entry_align;
        }
        return result;
    }

    // \brief Fit aligned storage in buffer.
    // Interface Similar to [ptr.align] but omits space computation.
    // The method can be removed and changed directly to an std::align
    // function call after dropping gcc < 5 support.
    inline void *align(std::size_t align, std::size_t, void *&ptr) const noexcept
    {
        const auto intptr = reinterpret_cast<uintptr_t>(ptr);
        const auto aligned = (intptr - 1u + align) & -align;
        return ptr = reinterpret_cast<void *>(aligned);
    }

    inline void *GetAlignedBlockPtr(void *ptr, BlockID bid) const
    {
        for (auto i = 0; i < bid; i++)
        {
            ptr = static_cast<char *>(ptr) + sizeof(CANARY);
            ptr = align(blocks[i].entry_align, blocks[i].entry_size, ptr);
            ptr = static_cast<char *>(ptr) + GetBlockSize((BlockID)i);
            ptr = static_cast<char *>(ptr) + sizeof(CANARY);
        }

        ptr = static_cast<char *>(ptr) + sizeof(CANARY);
        ptr = align(blocks[bid].entry_align, blocks[bid].entry_size, ptr);
        return ptr;
    }

    template <typename T> inline T *GetBlockEnd(char *shared_memory, BlockID bid) const
    {
        auto begin = GetBlockPtr<T>(shared_memory, bid);
        return begin + GetBlockEntries(bid);
    }

    template <typename T, bool WRITE_CANARY = false>
    inline T *GetBlockPtr(char *shared_memory, BlockID bid) const
    {
        char *ptr = (char *)GetAlignedBlockPtr(shared_memory, bid);
        if (WRITE_CANARY)
        {
            char *start_canary_ptr = ptr - sizeof(CANARY);
            char *end_canary_ptr = ptr + GetBlockSize(bid);
            std::copy(CANARY, CANARY + sizeof(CANARY), start_canary_ptr);
            std::copy(CANARY, CANARY + sizeof(CANARY), end_canary_ptr);
        }
        else
        {
            char *start_canary_ptr = ptr - sizeof(CANARY);
            char *end_canary_ptr = ptr + GetBlockSize(bid);
            bool start_canary_alive = std::equal(CANARY, CANARY + sizeof(CANARY), start_canary_ptr);
            bool end_canary_alive = std::equal(CANARY, CANARY + sizeof(CANARY), end_canary_ptr);
            if (!start_canary_alive)
            {
                throw util::exception("Start canary of block corrupted. (" +
                                      std::string(block_id_to_name[bid]) + ")" + SOURCE_REF);
            }
            if (!end_canary_alive)
            {
                throw util::exception("End canary of block corrupted. (" +
                                      std::string(block_id_to_name[bid]) + ")" + SOURCE_REF);
            }
        }

        return (T *)ptr;
    }
};

enum SharedDataType
{
    REGION_NONE,
    REGION_1,
    REGION_2
};

struct SharedDataTimestamp
{
    explicit SharedDataTimestamp(SharedDataType region, unsigned timestamp)
        : region(region), timestamp(timestamp)
    {
    }

    SharedDataType region;
    unsigned timestamp;

    static constexpr const char *name = "osrm-region";
};

inline std::string regionToString(const SharedDataType region)
{
    switch (region)
    {
    case REGION_1:
        return "REGION_1";
    case REGION_2:
        return "REGION_2";
    case REGION_NONE:
        return "REGION_NONE";
    default:
        return "INVALID_REGION";
    }
}

static_assert(sizeof(block_id_to_name) / sizeof(*block_id_to_name) == DataLayout::NUM_BLOCKS,
              "Number of blocks needs to match the number of Block names.");
}
}

#endif /* SHARED_DATA_TYPE_HPP */
