#include "storage/storage.hpp"

#include "storage/io.hpp"
#include "storage/shared_datatype.hpp"
#include "storage/shared_memory.hpp"
#include "storage/shared_memory_ownership.hpp"
#include "storage/shared_monitor.hpp"

#include "contractor/files.hpp"
#include "contractor/query_graph.hpp"

#include "customizer/edge_based_graph.hpp"
#include "customizer/files.hpp"

#include "extractor/class_data.hpp"
#include "extractor/compressed_edge_container.hpp"
#include "extractor/edge_based_edge.hpp"
#include "extractor/edge_based_node.hpp"
#include "extractor/files.hpp"
#include "extractor/maneuver_override.hpp"
#include "extractor/packed_osm_ids.hpp"
#include "extractor/profile_properties.hpp"
#include "extractor/query_node.hpp"
#include "extractor/travel_mode.hpp"

#include "guidance/files.hpp"
#include "guidance/turn_instruction.hpp"

#include "partitioner/cell_storage.hpp"
#include "partitioner/edge_based_graph_reader.hpp"
#include "partitioner/files.hpp"
#include "partitioner/multi_level_partition.hpp"

#include "engine/datafacade/datafacade_base.hpp"

#include "util/coordinate.hpp"
#include "util/exception.hpp"
#include "util/exception_utils.hpp"
#include "util/fingerprint.hpp"
#include "util/log.hpp"
#include "util/packed_vector.hpp"
#include "util/range_table.hpp"
#include "util/static_graph.hpp"
#include "util/static_rtree.hpp"
#include "util/typedefs.hpp"
#include "util/vector_view.hpp"

#ifdef __linux__
#include <sys/mman.h>
#endif

#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/interprocess/sync/scoped_lock.hpp>

#include <cstdint>

#include <fstream>
#include <iostream>
#include <iterator>
#include <new>
#include <string>

namespace osrm
{
namespace storage
{

static constexpr std::size_t NUM_METRICS = 8;

using RTreeLeaf = engine::datafacade::BaseDataFacade::RTreeLeaf;
using RTreeNode = util::StaticRTree<RTreeLeaf, storage::Ownership::View>::TreeNode;
using QueryGraph = util::StaticGraph<contractor::QueryEdge::EdgeData>;
using EdgeBasedGraph = util::StaticGraph<extractor::EdgeBasedEdge::EdgeData>;

using Monitor = SharedMonitor<SharedDataTimestamp>;

Storage::Storage(StorageConfig config_) : config(std::move(config_)) {}

int Storage::Run(int max_wait)
{
    BOOST_ASSERT_MSG(config.IsValid(), "Invalid storage config");

    util::LogPolicy::GetInstance().Unmute();

    boost::filesystem::path lock_path =
        boost::filesystem::temp_directory_path() / "osrm-datastore.lock";
    if (!boost::filesystem::exists(lock_path))
    {
        boost::filesystem::ofstream ofs(lock_path);
    }

    boost::interprocess::file_lock file_lock(lock_path.string().c_str());
    boost::interprocess::scoped_lock<boost::interprocess::file_lock> datastore_lock(
        file_lock, boost::interprocess::defer_lock);

    if (!datastore_lock.try_lock())
    {
        util::UnbufferedLog(logWARNING) << "Data update in progress, waiting until it finishes... ";
        datastore_lock.lock();
        util::UnbufferedLog(logWARNING) << "ok.";
    }

#ifdef __linux__
    // try to disable swapping on Linux
    const bool lock_flags = MCL_CURRENT | MCL_FUTURE;
    if (-1 == mlockall(lock_flags))
    {
        util::Log(logWARNING) << "Could not request RAM lock";
    }
#endif

    // Get the next region ID and time stamp without locking shared barriers.
    // Because of datastore_lock the only write operation can occur sequentially later.
    Monitor monitor(SharedDataTimestamp{REGION_NONE, 0});
    auto in_use_region = monitor.data().region;
    auto next_timestamp = monitor.data().timestamp + 1;
    auto next_region =
        in_use_region == REGION_2 || in_use_region == REGION_NONE ? REGION_1 : REGION_2;

    // ensure that the shared memory region we want to write to is really removed
    // this is only needef for failure recovery because we actually wait for all clients
    // to detach at the end of the function
    if (storage::SharedMemory::RegionExists(next_region))
    {
        util::Log(logWARNING) << "Old shared memory region " << regionToString(next_region)
                              << " still exists.";
        util::UnbufferedLog() << "Retrying removal... ";
        storage::SharedMemory::Remove(next_region);
        util::UnbufferedLog() << "ok.";
    }

    util::Log() << "Loading data into " << regionToString(next_region);

    // Populate a memory layout into stack memory
    DataLayout layout;
    PopulateLayout(layout);

    // Allocate shared memory block
    auto regions_size = sizeof(layout) + layout.GetSizeOfLayout();
    util::Log() << "Allocating shared memory of " << regions_size << " bytes";
    auto data_memory = makeSharedMemory(next_region, regions_size);

    // Copy memory layout to shared memory and populate data
    char *shared_memory_ptr = static_cast<char *>(data_memory->Ptr());
    memcpy(shared_memory_ptr, &layout, sizeof(layout));
    PopulateData(layout, shared_memory_ptr + sizeof(layout));

    { // Lock for write access shared region mutex
        boost::interprocess::scoped_lock<Monitor::mutex_type> lock(monitor.get_mutex(),
                                                                   boost::interprocess::defer_lock);

        if (max_wait >= 0)
        {
            if (!lock.timed_lock(boost::posix_time::microsec_clock::universal_time() +
                                 boost::posix_time::seconds(max_wait)))
            {
                util::Log(logWARNING)
                    << "Could not aquire current region lock after " << max_wait
                    << " seconds. Removing locked block and creating a new one. All currently "
                       "attached processes will not receive notifications and must be restarted";
                Monitor::remove();
                in_use_region = REGION_NONE;
                monitor = Monitor(SharedDataTimestamp{REGION_NONE, 0});
            }
        }
        else
        {
            lock.lock();
        }

        // Update the current region ID and timestamp
        monitor.data().region = next_region;
        monitor.data().timestamp = next_timestamp;
    }

    util::Log() << "All data loaded. Notify all client about new data in "
                << regionToString(next_region) << " with timestamp " << next_timestamp;
    monitor.notify_all();

    // SHMCTL(2): Mark the segment to be destroyed. The segment will actually be destroyed
    // only after the last process detaches it.
    if (in_use_region != REGION_NONE && storage::SharedMemory::RegionExists(in_use_region))
    {
        util::UnbufferedLog() << "Marking old shared memory region "
                              << regionToString(in_use_region) << " for removal... ";

        // aquire a handle for the old shared memory region before we mark it for deletion
        // we will need this to wait for all users to detach
        auto in_use_shared_memory = makeSharedMemory(in_use_region);

        storage::SharedMemory::Remove(in_use_region);
        util::UnbufferedLog() << "ok.";

        util::UnbufferedLog() << "Waiting for clients to detach... ";
        in_use_shared_memory->WaitForDetach();
        util::UnbufferedLog() << " ok.";
    }

    util::Log() << "All clients switched.";

    return EXIT_SUCCESS;
}

/**
 * This function examines all our data files and figures out how much
 * memory needs to be allocated, and the position of each data structure
 * in that big block.  It updates the fields in the DataLayout parameter.
 */
void Storage::PopulateLayout(DataLayout &layout)
{
    {
        auto absolute_file_index_path =
            boost::filesystem::absolute(config.GetPath(".osrm.fileIndex"));

        layout.SetBlock(DataLayout::FILE_INDEX_PATH,
                        make_block<char>(absolute_file_index_path.string().length() + 1));
    }

    {
        util::Log() << "load names from: " << config.GetPath(".osrm.names");
        // number of entries in name index
        io::FileReader name_file(config.GetPath(".osrm.names"), io::FileReader::VerifyFingerprint);
        layout.SetBlock(DataLayout::NAME_CHAR_DATA, make_block<char>(name_file.GetSize()));
    }

    {
        io::FileReader reader(config.GetPath(".osrm.tls"), io::FileReader::VerifyFingerprint);
        auto num_offsets = reader.ReadVectorSize<std::uint32_t>();
        auto num_masks = reader.ReadVectorSize<extractor::TurnLaneType::Mask>();

        layout.SetBlock(DataLayout::LANE_DESCRIPTION_OFFSETS,
                        make_block<std::uint32_t>(num_offsets));
        layout.SetBlock(DataLayout::LANE_DESCRIPTION_MASKS,
                        make_block<extractor::TurnLaneType::Mask>(num_masks));
    }

    // Loading information for original edges
    {
        io::FileReader edges_file(config.GetPath(".osrm.edges"), io::FileReader::VerifyFingerprint);
        const auto number_of_original_edges = edges_file.ReadElementCount64();

        // note: settings this all to the same size is correct, we extract them from the same struct
        layout.SetBlock(DataLayout::PRE_TURN_BEARING,
                        make_block<guidance::TurnBearing>(number_of_original_edges));
        layout.SetBlock(DataLayout::POST_TURN_BEARING,
                        make_block<guidance::TurnBearing>(number_of_original_edges));
        layout.SetBlock(DataLayout::TURN_INSTRUCTION,
                        make_block<guidance::TurnInstruction>(number_of_original_edges));
        layout.SetBlock(DataLayout::LANE_DATA_ID, make_block<LaneDataID>(number_of_original_edges));
        layout.SetBlock(DataLayout::ENTRY_CLASSID,
                        make_block<EntryClassID>(number_of_original_edges));
    }

    {
        io::FileReader nodes_data_file(config.GetPath(".osrm.ebg_nodes"),
                                       io::FileReader::VerifyFingerprint);
        const auto nodes_number = nodes_data_file.ReadElementCount64();
        const auto annotations_number = nodes_data_file.ReadElementCount64();
        layout.SetBlock(DataLayout::EDGE_BASED_NODE_DATA_LIST,
                        make_block<extractor::EdgeBasedNode>(nodes_number));
        layout.SetBlock(DataLayout::ANNOTATION_DATA_LIST,
                        make_block<extractor::NodeBasedEdgeAnnotation>(annotations_number));
    }

    if (boost::filesystem::exists(config.GetPath(".osrm.hsgr")))
    {
        io::FileReader reader(config.GetPath(".osrm.hsgr"), io::FileReader::VerifyFingerprint);

        reader.Skip<std::uint32_t>(1); // checksum
        auto num_nodes = reader.ReadVectorSize<contractor::QueryGraph::NodeArrayEntry>();
        auto num_edges = reader.ReadVectorSize<contractor::QueryGraph::EdgeArrayEntry>();
        auto num_metrics = reader.ReadElementCount64();

        if (num_metrics > NUM_METRICS)
        {
            throw util::exception("Only " + std::to_string(NUM_METRICS) +
                                  " metrics are supported at the same time.");
        }

        layout.SetBlock(DataLayout::HSGR_CHECKSUM, make_block<unsigned>(1));
        layout.SetBlock(DataLayout::CH_GRAPH_NODE_LIST,
                        make_block<contractor::QueryGraph::NodeArrayEntry>(num_nodes));
        layout.SetBlock(DataLayout::CH_GRAPH_EDGE_LIST,
                        make_block<contractor::QueryGraph::EdgeArrayEntry>(num_edges));

        for (const auto index : util::irange<std::size_t>(0, num_metrics))
        {
            layout.SetBlock(static_cast<DataLayout::BlockID>(DataLayout::CH_EDGE_FILTER_0 + index),
                            make_block<unsigned>(num_edges));
        }
        for (const auto index : util::irange<std::size_t>(num_metrics, NUM_METRICS))
        {
            layout.SetBlock(static_cast<DataLayout::BlockID>(DataLayout::CH_EDGE_FILTER_0 + index),
                            make_block<unsigned>(0));
        }
    }
    else
    {
        layout.SetBlock(DataLayout::HSGR_CHECKSUM, make_block<unsigned>(0));
        layout.SetBlock(DataLayout::CH_GRAPH_NODE_LIST,
                        make_block<contractor::QueryGraph::NodeArrayEntry>(0));
        layout.SetBlock(DataLayout::CH_GRAPH_EDGE_LIST,
                        make_block<contractor::QueryGraph::EdgeArrayEntry>(0));
        for (const auto index : util::irange<std::size_t>(0, NUM_METRICS))
        {
            layout.SetBlock(static_cast<DataLayout::BlockID>(DataLayout::CH_EDGE_FILTER_0 + index),
                            make_block<unsigned>(0));
        }
    }

    // load rsearch tree size
    {
        io::FileReader tree_node_file(config.GetPath(".osrm.ramIndex"),
                                      io::FileReader::VerifyFingerprint);

        const auto tree_size = tree_node_file.ReadElementCount64();
        layout.SetBlock(DataLayout::R_SEARCH_TREE, make_block<RTreeNode>(tree_size));
        tree_node_file.Skip<RTreeNode>(tree_size);
        const auto tree_levels_size = tree_node_file.ReadElementCount64();
        layout.SetBlock(DataLayout::R_SEARCH_TREE_LEVELS,
                        make_block<std::uint64_t>(tree_levels_size));
    }

    {
        layout.SetBlock(DataLayout::PROPERTIES, make_block<extractor::ProfileProperties>(1));
    }

    // read timestampsize
    {
        io::FileReader timestamp_file(config.GetPath(".osrm.timestamp"),
                                      io::FileReader::VerifyFingerprint);
        const auto timestamp_size = timestamp_file.GetSize();
        layout.SetBlock(DataLayout::TIMESTAMP, make_block<char>(timestamp_size));
    }

    // load turn weight penalties
    {
        io::FileReader turn_weight_penalties_file(config.GetPath(".osrm.turn_weight_penalties"),
                                                  io::FileReader::VerifyFingerprint);
        const auto number_of_penalties = turn_weight_penalties_file.ReadElementCount64();
        layout.SetBlock(DataLayout::TURN_WEIGHT_PENALTIES,
                        make_block<TurnPenalty>(number_of_penalties));
    }

    // load turn duration penalties
    {
        io::FileReader turn_duration_penalties_file(config.GetPath(".osrm.turn_duration_penalties"),
                                                    io::FileReader::VerifyFingerprint);
        const auto number_of_penalties = turn_duration_penalties_file.ReadElementCount64();
        layout.SetBlock(DataLayout::TURN_DURATION_PENALTIES,
                        make_block<TurnPenalty>(number_of_penalties));
    }

    // load coordinate size
    {
        io::FileReader node_file(config.GetPath(".osrm.nbg_nodes"),
                                 io::FileReader::VerifyFingerprint);
        const auto coordinate_list_size = node_file.ReadElementCount64();
        layout.SetBlock(DataLayout::COORDINATE_LIST,
                        make_block<util::Coordinate>(coordinate_list_size));
        node_file.Skip<util::Coordinate>(coordinate_list_size);
        // skip number of elements
        node_file.Skip<std::uint64_t>(1);
        const auto num_id_blocks = node_file.ReadElementCount64();
        // we'll read a list of OSM node IDs from the same data, so set the block size for the same
        // number of items:
        layout.SetBlock(DataLayout::OSM_NODE_ID_LIST,
                        make_block<extractor::PackedOSMIDsView::block_type>(num_id_blocks));
    }

    // load geometries sizes
    {
        io::FileReader reader(config.GetPath(".osrm.geometry"), io::FileReader::VerifyFingerprint);

        const auto number_of_geometries_indices = reader.ReadVectorSize<unsigned>();
        layout.SetBlock(DataLayout::GEOMETRIES_INDEX,
                        make_block<unsigned>(number_of_geometries_indices));

        const auto number_of_compressed_geometries = reader.ReadVectorSize<NodeID>();
        layout.SetBlock(DataLayout::GEOMETRIES_NODE_LIST,
                        make_block<NodeID>(number_of_compressed_geometries));

        reader.ReadElementCount64(); // number of segments
        const auto number_of_segment_weight_blocks =
            reader.ReadVectorSize<extractor::SegmentDataView::SegmentWeightVector::block_type>();

        reader.ReadElementCount64(); // number of segments
        auto number_of_rev_weight_blocks =
            reader.ReadVectorSize<extractor::SegmentDataView::SegmentWeightVector::block_type>();
        BOOST_ASSERT(number_of_rev_weight_blocks == number_of_segment_weight_blocks);
        (void)number_of_rev_weight_blocks;

        reader.ReadElementCount64(); // number of segments
        const auto number_of_segment_duration_blocks =
            reader.ReadVectorSize<extractor::SegmentDataView::SegmentDurationVector::block_type>();

        layout.SetBlock(DataLayout::GEOMETRIES_FWD_WEIGHT_LIST,
                        make_block<extractor::SegmentDataView::SegmentWeightVector::block_type>(
                            number_of_segment_weight_blocks));
        layout.SetBlock(DataLayout::GEOMETRIES_REV_WEIGHT_LIST,
                        make_block<extractor::SegmentDataView::SegmentWeightVector::block_type>(
                            number_of_segment_weight_blocks));
        layout.SetBlock(DataLayout::GEOMETRIES_FWD_DURATION_LIST,
                        make_block<extractor::SegmentDataView::SegmentDurationVector::block_type>(
                            number_of_segment_duration_blocks));
        layout.SetBlock(DataLayout::GEOMETRIES_REV_DURATION_LIST,
                        make_block<extractor::SegmentDataView::SegmentDurationVector::block_type>(
                            number_of_segment_duration_blocks));
        layout.SetBlock(DataLayout::GEOMETRIES_FWD_DATASOURCES_LIST,
                        make_block<DatasourceID>(number_of_compressed_geometries));
        layout.SetBlock(DataLayout::GEOMETRIES_REV_DATASOURCES_LIST,
                        make_block<DatasourceID>(number_of_compressed_geometries));
    }

    // Load datasource name sizes.
    {
        layout.SetBlock(DataLayout::DATASOURCES_NAMES, make_block<extractor::Datasources>(1));
    }

    {
        io::FileReader reader(config.GetPath(".osrm.icd"), io::FileReader::VerifyFingerprint);

        auto num_discreate_bearings = reader.ReadVectorSize<DiscreteBearing>();
        layout.SetBlock(DataLayout::BEARING_VALUES,
                        make_block<DiscreteBearing>(num_discreate_bearings));

        auto num_bearing_classes = reader.ReadVectorSize<BearingClassID>();
        layout.SetBlock(DataLayout::BEARING_CLASSID,
                        make_block<BearingClassID>(num_bearing_classes));

        reader.Skip<std::uint32_t>(1); // sum_lengths
        const auto bearing_blocks = reader.ReadVectorSize<unsigned>();
        const auto bearing_offsets =
            reader
                .ReadVectorSize<typename util::RangeTable<16, storage::Ownership::View>::BlockT>();

        layout.SetBlock(DataLayout::BEARING_OFFSETS, make_block<unsigned>(bearing_blocks));
        layout.SetBlock(DataLayout::BEARING_BLOCKS,
                        make_block<typename util::RangeTable<16, storage::Ownership::View>::BlockT>(
                            bearing_offsets));

        auto num_entry_classes = reader.ReadVectorSize<util::guidance::EntryClass>();
        layout.SetBlock(DataLayout::ENTRY_CLASS,
                        make_block<util::guidance::EntryClass>(num_entry_classes));
    }

    {
        // Loading turn lane data
        io::FileReader lane_data_file(config.GetPath(".osrm.tld"),
                                      io::FileReader::VerifyFingerprint);
        const auto lane_tuple_count = lane_data_file.ReadElementCount64();
        layout.SetBlock(DataLayout::TURN_LANE_DATA,
                        make_block<util::guidance::LaneTupleIdPair>(lane_tuple_count));
    }

    // load maneuver overrides
    {
        io::FileReader maneuver_overrides_file(config.GetPath(".osrm.maneuver_overrides"),
                                               io::FileReader::VerifyFingerprint);
        const auto number_of_overrides =
            maneuver_overrides_file.ReadVectorSize<extractor::StorageManeuverOverride>();
        layout.SetBlock(DataLayout::MANEUVER_OVERRIDES,
                        make_block<extractor::StorageManeuverOverride>(number_of_overrides));
        const auto number_of_nodes = maneuver_overrides_file.ReadVectorSize<NodeID>();
        layout.SetBlock(DataLayout::MANEUVER_OVERRIDE_NODE_SEQUENCES,
                        make_block<NodeID>(number_of_nodes));
    }

    {
        // Loading MLD Data
        if (boost::filesystem::exists(config.GetPath(".osrm.partition")))
        {
            io::FileReader reader(config.GetPath(".osrm.partition"),
                                  io::FileReader::VerifyFingerprint);

            reader.Skip<partitioner::MultiLevelPartition::LevelData>(1);
            layout.SetBlock(DataLayout::MLD_LEVEL_DATA,
                            make_block<partitioner::MultiLevelPartition::LevelData>(1));
            const auto partition_entries_count = reader.ReadVectorSize<PartitionID>();
            layout.SetBlock(DataLayout::MLD_PARTITION,
                            make_block<PartitionID>(partition_entries_count));
            const auto children_entries_count = reader.ReadVectorSize<CellID>();
            layout.SetBlock(DataLayout::MLD_CELL_TO_CHILDREN,
                            make_block<CellID>(children_entries_count));
        }
        else
        {
            layout.SetBlock(DataLayout::MLD_LEVEL_DATA,
                            make_block<partitioner::MultiLevelPartition::LevelData>(0));
            layout.SetBlock(DataLayout::MLD_PARTITION, make_block<PartitionID>(0));
            layout.SetBlock(DataLayout::MLD_CELL_TO_CHILDREN, make_block<CellID>(0));
        }

        if (boost::filesystem::exists(config.GetPath(".osrm.cells")))
        {
            io::FileReader reader(config.GetPath(".osrm.cells"), io::FileReader::VerifyFingerprint);

            const auto source_node_count = reader.ReadVectorSize<NodeID>();
            layout.SetBlock(DataLayout::MLD_CELL_SOURCE_BOUNDARY,
                            make_block<NodeID>(source_node_count));
            const auto destination_node_count = reader.ReadVectorSize<NodeID>();
            layout.SetBlock(DataLayout::MLD_CELL_DESTINATION_BOUNDARY,
                            make_block<NodeID>(destination_node_count));
            const auto cell_count = reader.ReadVectorSize<partitioner::CellStorage::CellData>();
            layout.SetBlock(DataLayout::MLD_CELLS,
                            make_block<partitioner::CellStorage::CellData>(cell_count));
            const auto level_offsets_count = reader.ReadVectorSize<std::uint64_t>();
            layout.SetBlock(DataLayout::MLD_CELL_LEVEL_OFFSETS,
                            make_block<std::uint64_t>(level_offsets_count));
        }
        else
        {
            layout.SetBlock(DataLayout::MLD_CELL_SOURCE_BOUNDARY, make_block<char>(0));
            layout.SetBlock(DataLayout::MLD_CELL_DESTINATION_BOUNDARY, make_block<char>(0));
            layout.SetBlock(DataLayout::MLD_CELLS, make_block<char>(0));
            layout.SetBlock(DataLayout::MLD_CELL_LEVEL_OFFSETS, make_block<char>(0));
        }

        if (boost::filesystem::exists(config.GetPath(".osrm.cell_metrics")))
        {
            io::FileReader reader(config.GetPath(".osrm.cell_metrics"),
                                  io::FileReader::VerifyFingerprint);
            auto num_metric = reader.ReadElementCount64();

            if (num_metric > NUM_METRICS)
            {
                throw util::exception("Only " + std::to_string(NUM_METRICS) +
                                      " metrics are supported at the same time.");
            }

            for (const auto index : util::irange<std::size_t>(0, num_metric))
            {
                const auto weights_count = reader.ReadVectorSize<EdgeWeight>();
                layout.SetBlock(
                    static_cast<DataLayout::BlockID>(DataLayout::MLD_CELL_WEIGHTS_0 + index),
                    make_block<EdgeWeight>(weights_count));
                const auto durations_count = reader.ReadVectorSize<EdgeDuration>();
                layout.SetBlock(
                    static_cast<DataLayout::BlockID>(DataLayout::MLD_CELL_DURATIONS_0 + index),
                    make_block<EdgeDuration>(durations_count));
            }
            for (const auto index : util::irange<std::size_t>(num_metric, NUM_METRICS))
            {
                layout.SetBlock(
                    static_cast<DataLayout::BlockID>(DataLayout::MLD_CELL_WEIGHTS_0 + index),
                    make_block<EdgeWeight>(0));
                layout.SetBlock(
                    static_cast<DataLayout::BlockID>(DataLayout::MLD_CELL_DURATIONS_0 + index),
                    make_block<EdgeDuration>(0));
            }
        }
        else
        {
            layout.SetBlock(DataLayout::MLD_CELL_WEIGHTS_0, make_block<char>(0));
            layout.SetBlock(DataLayout::MLD_CELL_WEIGHTS_1, make_block<char>(0));
            layout.SetBlock(DataLayout::MLD_CELL_WEIGHTS_2, make_block<char>(0));
            layout.SetBlock(DataLayout::MLD_CELL_WEIGHTS_3, make_block<char>(0));
            layout.SetBlock(DataLayout::MLD_CELL_WEIGHTS_4, make_block<char>(0));
            layout.SetBlock(DataLayout::MLD_CELL_WEIGHTS_5, make_block<char>(0));
            layout.SetBlock(DataLayout::MLD_CELL_WEIGHTS_6, make_block<char>(0));
            layout.SetBlock(DataLayout::MLD_CELL_WEIGHTS_7, make_block<char>(0));
            layout.SetBlock(DataLayout::MLD_CELL_DURATIONS_0, make_block<char>(0));
            layout.SetBlock(DataLayout::MLD_CELL_DURATIONS_1, make_block<char>(0));
            layout.SetBlock(DataLayout::MLD_CELL_DURATIONS_2, make_block<char>(0));
            layout.SetBlock(DataLayout::MLD_CELL_DURATIONS_3, make_block<char>(0));
            layout.SetBlock(DataLayout::MLD_CELL_DURATIONS_4, make_block<char>(0));
            layout.SetBlock(DataLayout::MLD_CELL_DURATIONS_5, make_block<char>(0));
            layout.SetBlock(DataLayout::MLD_CELL_DURATIONS_6, make_block<char>(0));
            layout.SetBlock(DataLayout::MLD_CELL_DURATIONS_7, make_block<char>(0));
        }

        if (boost::filesystem::exists(config.GetPath(".osrm.mldgr")))
        {
            io::FileReader reader(config.GetPath(".osrm.mldgr"), io::FileReader::VerifyFingerprint);

            const auto num_nodes =
                reader.ReadVectorSize<customizer::MultiLevelEdgeBasedGraph::NodeArrayEntry>();
            const auto num_edges =
                reader.ReadVectorSize<customizer::MultiLevelEdgeBasedGraph::EdgeArrayEntry>();
            const auto num_node_offsets =
                reader.ReadVectorSize<customizer::MultiLevelEdgeBasedGraph::EdgeOffset>();

            layout.SetBlock(
                DataLayout::MLD_GRAPH_NODE_LIST,
                make_block<customizer::MultiLevelEdgeBasedGraph::NodeArrayEntry>(num_nodes));
            layout.SetBlock(
                DataLayout::MLD_GRAPH_EDGE_LIST,
                make_block<customizer::MultiLevelEdgeBasedGraph::EdgeArrayEntry>(num_edges));
            layout.SetBlock(
                DataLayout::MLD_GRAPH_NODE_TO_OFFSET,
                make_block<customizer::MultiLevelEdgeBasedGraph::EdgeOffset>(num_node_offsets));
        }
        else
        {
            layout.SetBlock(DataLayout::MLD_GRAPH_NODE_LIST,
                            make_block<customizer::MultiLevelEdgeBasedGraph::NodeArrayEntry>(0));
            layout.SetBlock(DataLayout::MLD_GRAPH_EDGE_LIST,
                            make_block<customizer::MultiLevelEdgeBasedGraph::EdgeArrayEntry>(0));
            layout.SetBlock(DataLayout::MLD_GRAPH_NODE_TO_OFFSET,
                            make_block<customizer::MultiLevelEdgeBasedGraph::EdgeOffset>(0));
        }
    }
}

void Storage::PopulateData(const DataLayout &layout, char *memory_ptr)
{
    BOOST_ASSERT(memory_ptr != nullptr);

    // Connectivity matrix checksum
    std::uint32_t turns_connectivity_checksum = 0;

    // read actual data into shared memory object //

    // store the filename of the on-disk portion of the RTree
    {
        const auto file_index_path_ptr =
            layout.GetBlockPtr<char, true>(memory_ptr, DataLayout::FILE_INDEX_PATH);
        // make sure we have 0 ending
        std::fill(file_index_path_ptr,
                  file_index_path_ptr + layout.GetBlockSize(DataLayout::FILE_INDEX_PATH),
                  0);
        const auto absolute_file_index_path =
            boost::filesystem::absolute(config.GetPath(".osrm.fileIndex")).string();
        BOOST_ASSERT(static_cast<std::size_t>(layout.GetBlockSize(DataLayout::FILE_INDEX_PATH)) >=
                     absolute_file_index_path.size());
        std::copy(
            absolute_file_index_path.begin(), absolute_file_index_path.end(), file_index_path_ptr);
    }

    // Name data
    {
        io::FileReader name_file(config.GetPath(".osrm.names"), io::FileReader::VerifyFingerprint);
        std::size_t name_file_size = name_file.GetSize();

        BOOST_ASSERT(name_file_size == layout.GetBlockSize(DataLayout::NAME_CHAR_DATA));
        const auto name_char_ptr =
            layout.GetBlockPtr<char, true>(memory_ptr, DataLayout::NAME_CHAR_DATA);

        name_file.ReadInto<char>(name_char_ptr, name_file_size);
    }

    // Turn lane data
    {
        io::FileReader lane_data_file(config.GetPath(".osrm.tld"),
                                      io::FileReader::VerifyFingerprint);

        const auto lane_tuple_count = lane_data_file.ReadElementCount64();

        // Need to call GetBlockPtr -> it write the memory canary, even if no data needs to be
        // loaded.
        const auto turn_lane_data_ptr = layout.GetBlockPtr<util::guidance::LaneTupleIdPair, true>(
            memory_ptr, DataLayout::TURN_LANE_DATA);
        BOOST_ASSERT(lane_tuple_count * sizeof(util::guidance::LaneTupleIdPair) ==
                     layout.GetBlockSize(DataLayout::TURN_LANE_DATA));
        lane_data_file.ReadInto(turn_lane_data_ptr, lane_tuple_count);
    }

    // Turn lane descriptions
    {
        auto offsets_ptr = layout.GetBlockPtr<std::uint32_t, true>(
            memory_ptr, storage::DataLayout::LANE_DESCRIPTION_OFFSETS);
        util::vector_view<std::uint32_t> offsets(
            offsets_ptr, layout.GetBlockEntries(storage::DataLayout::LANE_DESCRIPTION_OFFSETS));

        auto masks_ptr = layout.GetBlockPtr<extractor::TurnLaneType::Mask, true>(
            memory_ptr, storage::DataLayout::LANE_DESCRIPTION_MASKS);
        util::vector_view<extractor::TurnLaneType::Mask> masks(
            masks_ptr, layout.GetBlockEntries(storage::DataLayout::LANE_DESCRIPTION_MASKS));

        extractor::files::readTurnLaneDescriptions(config.GetPath(".osrm.tls"), offsets, masks);
    }

    // Load edge-based nodes data
    {
        auto edge_based_node_data_list_ptr = layout.GetBlockPtr<extractor::EdgeBasedNode, true>(
            memory_ptr, storage::DataLayout::EDGE_BASED_NODE_DATA_LIST);
        util::vector_view<extractor::EdgeBasedNode> edge_based_node_data(
            edge_based_node_data_list_ptr,
            layout.GetBlockEntries(storage::DataLayout::EDGE_BASED_NODE_DATA_LIST));

        auto annotation_data_list_ptr =
            layout.GetBlockPtr<extractor::NodeBasedEdgeAnnotation, true>(
                memory_ptr, storage::DataLayout::ANNOTATION_DATA_LIST);
        util::vector_view<extractor::NodeBasedEdgeAnnotation> annotation_data(
            annotation_data_list_ptr,
            layout.GetBlockEntries(storage::DataLayout::ANNOTATION_DATA_LIST));

        extractor::EdgeBasedNodeDataView node_data(std::move(edge_based_node_data),
                                                   std::move(annotation_data));

        extractor::files::readNodeData(config.GetPath(".osrm.ebg_nodes"), node_data);
    }

    // Load original edge data
    {
        const auto lane_data_id_ptr =
            layout.GetBlockPtr<LaneDataID, true>(memory_ptr, storage::DataLayout::LANE_DATA_ID);
        util::vector_view<LaneDataID> lane_data_ids(
            lane_data_id_ptr, layout.GetBlockEntries(storage::DataLayout::LANE_DATA_ID));

        const auto turn_instruction_list_ptr = layout.GetBlockPtr<guidance::TurnInstruction, true>(
            memory_ptr, storage::DataLayout::TURN_INSTRUCTION);
        util::vector_view<guidance::TurnInstruction> turn_instructions(
            turn_instruction_list_ptr,
            layout.GetBlockEntries(storage::DataLayout::TURN_INSTRUCTION));

        const auto entry_class_id_list_ptr =
            layout.GetBlockPtr<EntryClassID, true>(memory_ptr, storage::DataLayout::ENTRY_CLASSID);
        util::vector_view<EntryClassID> entry_class_ids(
            entry_class_id_list_ptr, layout.GetBlockEntries(storage::DataLayout::ENTRY_CLASSID));

        const auto pre_turn_bearing_ptr = layout.GetBlockPtr<guidance::TurnBearing, true>(
            memory_ptr, storage::DataLayout::PRE_TURN_BEARING);
        util::vector_view<guidance::TurnBearing> pre_turn_bearings(
            pre_turn_bearing_ptr, layout.GetBlockEntries(storage::DataLayout::PRE_TURN_BEARING));

        const auto post_turn_bearing_ptr = layout.GetBlockPtr<guidance::TurnBearing, true>(
            memory_ptr, storage::DataLayout::POST_TURN_BEARING);
        util::vector_view<guidance::TurnBearing> post_turn_bearings(
            post_turn_bearing_ptr, layout.GetBlockEntries(storage::DataLayout::POST_TURN_BEARING));

        guidance::TurnDataView turn_data(std::move(turn_instructions),
                                         std::move(lane_data_ids),
                                         std::move(entry_class_ids),
                                         std::move(pre_turn_bearings),
                                         std::move(post_turn_bearings));

        guidance::files::readTurnData(
            config.GetPath(".osrm.edges"), turn_data, turns_connectivity_checksum);
    }

    // load compressed geometry
    {
        auto geometries_index_ptr =
            layout.GetBlockPtr<unsigned, true>(memory_ptr, storage::DataLayout::GEOMETRIES_INDEX);
        util::vector_view<unsigned> geometry_begin_indices(
            geometries_index_ptr, layout.GetBlockEntries(storage::DataLayout::GEOMETRIES_INDEX));

        auto num_entries = layout.GetBlockEntries(storage::DataLayout::GEOMETRIES_NODE_LIST);

        auto geometries_node_list_ptr =
            layout.GetBlockPtr<NodeID, true>(memory_ptr, storage::DataLayout::GEOMETRIES_NODE_LIST);
        util::vector_view<NodeID> geometry_node_list(geometries_node_list_ptr, num_entries);

        auto geometries_fwd_weight_list_ptr =
            layout.GetBlockPtr<extractor::SegmentDataView::SegmentWeightVector::block_type, true>(
                memory_ptr, storage::DataLayout::GEOMETRIES_FWD_WEIGHT_LIST);
        extractor::SegmentDataView::SegmentWeightVector geometry_fwd_weight_list(
            util::vector_view<extractor::SegmentDataView::SegmentWeightVector::block_type>(
                geometries_fwd_weight_list_ptr,
                layout.GetBlockEntries(storage::DataLayout::GEOMETRIES_FWD_WEIGHT_LIST)),
            num_entries);

        auto geometries_rev_weight_list_ptr =
            layout.GetBlockPtr<extractor::SegmentDataView::SegmentWeightVector::block_type, true>(
                memory_ptr, storage::DataLayout::GEOMETRIES_REV_WEIGHT_LIST);
        extractor::SegmentDataView::SegmentWeightVector geometry_rev_weight_list(
            util::vector_view<extractor::SegmentDataView::SegmentWeightVector::block_type>(
                geometries_rev_weight_list_ptr,
                layout.GetBlockEntries(storage::DataLayout::GEOMETRIES_REV_WEIGHT_LIST)),
            num_entries);

        auto geometries_fwd_duration_list_ptr =
            layout.GetBlockPtr<extractor::SegmentDataView::SegmentDurationVector::block_type, true>(
                memory_ptr, storage::DataLayout::GEOMETRIES_FWD_DURATION_LIST);
        extractor::SegmentDataView::SegmentDurationVector geometry_fwd_duration_list(
            util::vector_view<extractor::SegmentDataView::SegmentDurationVector::block_type>(
                geometries_fwd_duration_list_ptr,
                layout.GetBlockEntries(storage::DataLayout::GEOMETRIES_FWD_DURATION_LIST)),
            num_entries);

        auto geometries_rev_duration_list_ptr =
            layout.GetBlockPtr<extractor::SegmentDataView::SegmentDurationVector::block_type, true>(
                memory_ptr, storage::DataLayout::GEOMETRIES_REV_DURATION_LIST);
        extractor::SegmentDataView::SegmentDurationVector geometry_rev_duration_list(
            util::vector_view<extractor::SegmentDataView::SegmentDurationVector::block_type>(
                geometries_rev_duration_list_ptr,
                layout.GetBlockEntries(storage::DataLayout::GEOMETRIES_REV_DURATION_LIST)),
            num_entries);

        auto geometries_fwd_datasources_list_ptr = layout.GetBlockPtr<DatasourceID, true>(
            memory_ptr, storage::DataLayout::GEOMETRIES_FWD_DATASOURCES_LIST);
        util::vector_view<DatasourceID> geometry_fwd_datasources_list(
            geometries_fwd_datasources_list_ptr,
            layout.GetBlockEntries(storage::DataLayout::GEOMETRIES_FWD_DATASOURCES_LIST));

        auto geometries_rev_datasources_list_ptr = layout.GetBlockPtr<DatasourceID, true>(
            memory_ptr, storage::DataLayout::GEOMETRIES_REV_DATASOURCES_LIST);
        util::vector_view<DatasourceID> geometry_rev_datasources_list(
            geometries_rev_datasources_list_ptr,
            layout.GetBlockEntries(storage::DataLayout::GEOMETRIES_REV_DATASOURCES_LIST));

        extractor::SegmentDataView segment_data{std::move(geometry_begin_indices),
                                                std::move(geometry_node_list),
                                                std::move(geometry_fwd_weight_list),
                                                std::move(geometry_rev_weight_list),
                                                std::move(geometry_fwd_duration_list),
                                                std::move(geometry_rev_duration_list),
                                                std::move(geometry_fwd_datasources_list),
                                                std::move(geometry_rev_datasources_list)};

        extractor::files::readSegmentData(config.GetPath(".osrm.geometry"), segment_data);
    }

    {
        const auto datasources_names_ptr = layout.GetBlockPtr<extractor::Datasources, true>(
            memory_ptr, DataLayout::DATASOURCES_NAMES);
        extractor::files::readDatasources(config.GetPath(".osrm.datasource_names"),
                                          *datasources_names_ptr);
    }

    // Loading list of coordinates
    {
        const auto coordinates_ptr =
            layout.GetBlockPtr<util::Coordinate, true>(memory_ptr, DataLayout::COORDINATE_LIST);
        const auto osmnodeid_ptr =
            layout.GetBlockPtr<extractor::PackedOSMIDsView::block_type, true>(
                memory_ptr, DataLayout::OSM_NODE_ID_LIST);
        util::vector_view<util::Coordinate> coordinates(
            coordinates_ptr, layout.GetBlockEntries(DataLayout::COORDINATE_LIST));
        extractor::PackedOSMIDsView osm_node_ids(
            util::vector_view<extractor::PackedOSMIDsView::block_type>(
                osmnodeid_ptr, layout.GetBlockEntries(DataLayout::OSM_NODE_ID_LIST)),
            layout.GetBlockEntries(DataLayout::COORDINATE_LIST));

        extractor::files::readNodes(config.GetPath(".osrm.nbg_nodes"), coordinates, osm_node_ids);
    }

    // load turn weight penalties
    {
        io::FileReader turn_weight_penalties_file(config.GetPath(".osrm.turn_weight_penalties"),
                                                  io::FileReader::VerifyFingerprint);
        const auto number_of_penalties = turn_weight_penalties_file.ReadElementCount64();
        const auto turn_weight_penalties_ptr =
            layout.GetBlockPtr<TurnPenalty, true>(memory_ptr, DataLayout::TURN_WEIGHT_PENALTIES);
        turn_weight_penalties_file.ReadInto(turn_weight_penalties_ptr, number_of_penalties);
    }

    // load turn duration penalties
    {
        io::FileReader turn_duration_penalties_file(config.GetPath(".osrm.turn_duration_penalties"),
                                                    io::FileReader::VerifyFingerprint);
        const auto number_of_penalties = turn_duration_penalties_file.ReadElementCount64();
        const auto turn_duration_penalties_ptr =
            layout.GetBlockPtr<TurnPenalty, true>(memory_ptr, DataLayout::TURN_DURATION_PENALTIES);
        turn_duration_penalties_file.ReadInto(turn_duration_penalties_ptr, number_of_penalties);
    }

    // store timestamp
    {
        io::FileReader timestamp_file(config.GetPath(".osrm.timestamp"),
                                      io::FileReader::VerifyFingerprint);
        const auto timestamp_size = timestamp_file.GetSize();

        const auto timestamp_ptr =
            layout.GetBlockPtr<char, true>(memory_ptr, DataLayout::TIMESTAMP);
        BOOST_ASSERT(timestamp_size == layout.GetBlockEntries(DataLayout::TIMESTAMP));
        timestamp_file.ReadInto(timestamp_ptr, timestamp_size);
    }

    // store search tree portion of rtree
    {
        io::FileReader tree_node_file(config.GetPath(".osrm.ramIndex"),
                                      io::FileReader::VerifyFingerprint);
        // perform this read so that we're at the right stream position for the next
        // read.
        tree_node_file.Skip<std::uint64_t>(1);
        const auto rtree_ptr =
            layout.GetBlockPtr<RTreeNode, true>(memory_ptr, DataLayout::R_SEARCH_TREE);

        tree_node_file.ReadInto(rtree_ptr, layout.GetBlockEntries(DataLayout::R_SEARCH_TREE));

        tree_node_file.Skip<std::uint64_t>(1);
        const auto rtree_levelsizes_ptr =
            layout.GetBlockPtr<std::uint64_t, true>(memory_ptr, DataLayout::R_SEARCH_TREE_LEVELS);

        tree_node_file.ReadInto(rtree_levelsizes_ptr,
                                layout.GetBlockEntries(DataLayout::R_SEARCH_TREE_LEVELS));
    }

    // load profile properties
    {
        const auto profile_properties_ptr = layout.GetBlockPtr<extractor::ProfileProperties, true>(
            memory_ptr, DataLayout::PROPERTIES);
        extractor::files::readProfileProperties(config.GetPath(".osrm.properties"),
                                                *profile_properties_ptr);
    }

    // Load intersection data
    {
        auto bearing_class_id_ptr = layout.GetBlockPtr<BearingClassID, true>(
            memory_ptr, storage::DataLayout::BEARING_CLASSID);
        util::vector_view<BearingClassID> bearing_class_id(
            bearing_class_id_ptr, layout.GetBlockEntries(storage::DataLayout::BEARING_CLASSID));

        auto bearing_values_ptr = layout.GetBlockPtr<DiscreteBearing, true>(
            memory_ptr, storage::DataLayout::BEARING_VALUES);
        util::vector_view<DiscreteBearing> bearing_values(
            bearing_values_ptr, layout.GetBlockEntries(storage::DataLayout::BEARING_VALUES));

        auto offsets_ptr =
            layout.GetBlockPtr<unsigned, true>(memory_ptr, storage::DataLayout::BEARING_OFFSETS);
        auto blocks_ptr =
            layout.GetBlockPtr<util::RangeTable<16, storage::Ownership::View>::BlockT, true>(
                memory_ptr, storage::DataLayout::BEARING_BLOCKS);
        util::vector_view<unsigned> bearing_offsets(
            offsets_ptr, layout.GetBlockEntries(storage::DataLayout::BEARING_OFFSETS));
        util::vector_view<util::RangeTable<16, storage::Ownership::View>::BlockT> bearing_blocks(
            blocks_ptr, layout.GetBlockEntries(storage::DataLayout::BEARING_BLOCKS));

        util::RangeTable<16, storage::Ownership::View> bearing_range_table(
            bearing_offsets, bearing_blocks, static_cast<unsigned>(bearing_values.size()));

        extractor::IntersectionBearingsView intersection_bearings_view{
            std::move(bearing_values), std::move(bearing_class_id), std::move(bearing_range_table)};

        auto entry_class_ptr = layout.GetBlockPtr<util::guidance::EntryClass, true>(
            memory_ptr, storage::DataLayout::ENTRY_CLASS);
        util::vector_view<util::guidance::EntryClass> entry_classes(
            entry_class_ptr, layout.GetBlockEntries(storage::DataLayout::ENTRY_CLASS));

        extractor::files::readIntersections(
            config.GetPath(".osrm.icd"), intersection_bearings_view, entry_classes);
    }

    { // Load the HSGR file
        if (boost::filesystem::exists(config.GetPath(".osrm.hsgr")))
        {
            auto graph_nodes_ptr =
                layout.GetBlockPtr<contractor::QueryGraphView::NodeArrayEntry, true>(
                    memory_ptr, storage::DataLayout::CH_GRAPH_NODE_LIST);
            auto graph_edges_ptr =
                layout.GetBlockPtr<contractor::QueryGraphView::EdgeArrayEntry, true>(
                    memory_ptr, storage::DataLayout::CH_GRAPH_EDGE_LIST);
            auto checksum =
                layout.GetBlockPtr<unsigned, true>(memory_ptr, DataLayout::HSGR_CHECKSUM);

            util::vector_view<contractor::QueryGraphView::NodeArrayEntry> node_list(
                graph_nodes_ptr, layout.GetBlockEntries(storage::DataLayout::CH_GRAPH_NODE_LIST));
            util::vector_view<contractor::QueryGraphView::EdgeArrayEntry> edge_list(
                graph_edges_ptr, layout.GetBlockEntries(storage::DataLayout::CH_GRAPH_EDGE_LIST));

            std::vector<util::vector_view<bool>> edge_filter;
            for (auto index : util::irange<std::size_t>(0, NUM_METRICS))
            {
                auto block_id =
                    static_cast<DataLayout::BlockID>(storage::DataLayout::CH_EDGE_FILTER_0 + index);
                auto data_ptr = layout.GetBlockPtr<unsigned, true>(memory_ptr, block_id);
                auto num_entries = layout.GetBlockEntries(block_id);
                edge_filter.emplace_back(data_ptr, num_entries);
            }

            std::uint32_t graph_connectivity_checksum = 0;
            contractor::QueryGraphView graph_view(std::move(node_list), std::move(edge_list));
            contractor::files::readGraph(config.GetPath(".osrm.hsgr"),
                                         *checksum,
                                         graph_view,
                                         edge_filter,
                                         graph_connectivity_checksum);
            if (turns_connectivity_checksum != graph_connectivity_checksum)
            {
                throw util::exception(
                    "Connectivity checksum " + std::to_string(graph_connectivity_checksum) +
                    " in " + config.GetPath(".osrm.hsgr").string() +
                    " does not equal to checksum " + std::to_string(turns_connectivity_checksum) +
                    " in " + config.GetPath(".osrm.edges").string());
            }
        }
        else
        {
            layout.GetBlockPtr<unsigned, true>(memory_ptr, DataLayout::HSGR_CHECKSUM);
            layout.GetBlockPtr<contractor::QueryGraphView::NodeArrayEntry, true>(
                memory_ptr, DataLayout::CH_GRAPH_NODE_LIST);
            layout.GetBlockPtr<contractor::QueryGraphView::EdgeArrayEntry, true>(
                memory_ptr, DataLayout::CH_GRAPH_EDGE_LIST);
        }
    }

    { // Loading MLD Data
        if (boost::filesystem::exists(config.GetPath(".osrm.partition")))
        {
            BOOST_ASSERT(layout.GetBlockSize(storage::DataLayout::MLD_LEVEL_DATA) > 0);
            BOOST_ASSERT(layout.GetBlockSize(storage::DataLayout::MLD_CELL_TO_CHILDREN) > 0);
            BOOST_ASSERT(layout.GetBlockSize(storage::DataLayout::MLD_PARTITION) > 0);

            auto level_data =
                layout.GetBlockPtr<partitioner::MultiLevelPartitionView::LevelData, true>(
                    memory_ptr, storage::DataLayout::MLD_LEVEL_DATA);

            auto mld_partition_ptr = layout.GetBlockPtr<PartitionID, true>(
                memory_ptr, storage::DataLayout::MLD_PARTITION);
            auto partition_entries_count =
                layout.GetBlockEntries(storage::DataLayout::MLD_PARTITION);
            util::vector_view<PartitionID> partition(mld_partition_ptr, partition_entries_count);

            auto mld_chilren_ptr = layout.GetBlockPtr<CellID, true>(
                memory_ptr, storage::DataLayout::MLD_CELL_TO_CHILDREN);
            auto children_entries_count =
                layout.GetBlockEntries(storage::DataLayout::MLD_CELL_TO_CHILDREN);
            util::vector_view<CellID> cell_to_children(mld_chilren_ptr, children_entries_count);

            partitioner::MultiLevelPartitionView mlp{
                std::move(level_data), std::move(partition), std::move(cell_to_children)};
            partitioner::files::readPartition(config.GetPath(".osrm.partition"), mlp);
        }

        if (boost::filesystem::exists(config.GetPath(".osrm.cells")))
        {
            BOOST_ASSERT(layout.GetBlockSize(storage::DataLayout::MLD_CELLS) > 0);
            BOOST_ASSERT(layout.GetBlockSize(storage::DataLayout::MLD_CELL_LEVEL_OFFSETS) > 0);

            auto mld_source_boundary_ptr = layout.GetBlockPtr<NodeID, true>(
                memory_ptr, storage::DataLayout::MLD_CELL_SOURCE_BOUNDARY);
            auto mld_destination_boundary_ptr = layout.GetBlockPtr<NodeID, true>(
                memory_ptr, storage::DataLayout::MLD_CELL_DESTINATION_BOUNDARY);
            auto mld_cells_ptr = layout.GetBlockPtr<partitioner::CellStorageView::CellData, true>(
                memory_ptr, storage::DataLayout::MLD_CELLS);
            auto mld_cell_level_offsets_ptr = layout.GetBlockPtr<std::uint64_t, true>(
                memory_ptr, storage::DataLayout::MLD_CELL_LEVEL_OFFSETS);

            auto source_boundary_entries_count =
                layout.GetBlockEntries(storage::DataLayout::MLD_CELL_SOURCE_BOUNDARY);
            auto destination_boundary_entries_count =
                layout.GetBlockEntries(storage::DataLayout::MLD_CELL_DESTINATION_BOUNDARY);
            auto cells_entries_counts = layout.GetBlockEntries(storage::DataLayout::MLD_CELLS);
            auto cell_level_offsets_entries_count =
                layout.GetBlockEntries(storage::DataLayout::MLD_CELL_LEVEL_OFFSETS);

            util::vector_view<NodeID> source_boundary(mld_source_boundary_ptr,
                                                      source_boundary_entries_count);
            util::vector_view<NodeID> destination_boundary(mld_destination_boundary_ptr,
                                                           destination_boundary_entries_count);
            util::vector_view<partitioner::CellStorageView::CellData> cells(mld_cells_ptr,
                                                                            cells_entries_counts);
            util::vector_view<std::uint64_t> level_offsets(mld_cell_level_offsets_ptr,
                                                           cell_level_offsets_entries_count);

            partitioner::CellStorageView storage{std::move(source_boundary),
                                                 std::move(destination_boundary),
                                                 std::move(cells),
                                                 std::move(level_offsets)};
            partitioner::files::readCells(config.GetPath(".osrm.cells"), storage);
        }

        if (boost::filesystem::exists(config.GetPath(".osrm.cell_metrics")))
        {
            BOOST_ASSERT(layout.GetBlockSize(storage::DataLayout::MLD_CELLS) > 0);
            BOOST_ASSERT(layout.GetBlockSize(storage::DataLayout::MLD_CELL_LEVEL_OFFSETS) > 0);

            std::vector<customizer::CellMetricView> metrics;

            for (auto index : util::irange<std::size_t>(0, NUM_METRICS))
            {
                auto weights_block_id = static_cast<DataLayout::BlockID>(
                    storage::DataLayout::MLD_CELL_WEIGHTS_0 + index);
                auto durations_block_id = static_cast<DataLayout::BlockID>(
                    storage::DataLayout::MLD_CELL_DURATIONS_0 + index);

                auto weight_entries_count = layout.GetBlockEntries(weights_block_id);
                auto duration_entries_count = layout.GetBlockEntries(durations_block_id);
                auto mld_cell_weights_ptr =
                    layout.GetBlockPtr<EdgeWeight, true>(memory_ptr, weights_block_id);
                auto mld_cell_duration_ptr =
                    layout.GetBlockPtr<EdgeDuration, true>(memory_ptr, durations_block_id);
                util::vector_view<EdgeWeight> weights(mld_cell_weights_ptr, weight_entries_count);
                util::vector_view<EdgeDuration> durations(mld_cell_duration_ptr,
                                                          duration_entries_count);

                metrics.push_back(
                    customizer::CellMetricView{std::move(weights), std::move(durations)});
            }

            customizer::files::readCellMetrics(config.GetPath(".osrm.cell_metrics"), metrics);
        }

        if (boost::filesystem::exists(config.GetPath(".osrm.mldgr")))
        {

            auto graph_nodes_ptr =
                layout.GetBlockPtr<customizer::MultiLevelEdgeBasedGraphView::NodeArrayEntry, true>(
                    memory_ptr, storage::DataLayout::MLD_GRAPH_NODE_LIST);
            auto graph_edges_ptr =
                layout.GetBlockPtr<customizer::MultiLevelEdgeBasedGraphView::EdgeArrayEntry, true>(
                    memory_ptr, storage::DataLayout::MLD_GRAPH_EDGE_LIST);
            auto graph_node_to_offset_ptr =
                layout.GetBlockPtr<customizer::MultiLevelEdgeBasedGraphView::EdgeOffset, true>(
                    memory_ptr, storage::DataLayout::MLD_GRAPH_NODE_TO_OFFSET);

            util::vector_view<customizer::MultiLevelEdgeBasedGraphView::NodeArrayEntry> node_list(
                graph_nodes_ptr, layout.GetBlockEntries(storage::DataLayout::MLD_GRAPH_NODE_LIST));
            util::vector_view<customizer::MultiLevelEdgeBasedGraphView::EdgeArrayEntry> edge_list(
                graph_edges_ptr, layout.GetBlockEntries(storage::DataLayout::MLD_GRAPH_EDGE_LIST));
            util::vector_view<customizer::MultiLevelEdgeBasedGraphView::EdgeOffset> node_to_offset(
                graph_node_to_offset_ptr,
                layout.GetBlockEntries(storage::DataLayout::MLD_GRAPH_NODE_TO_OFFSET));

            std::uint32_t graph_connectivity_checksum = 0;
            customizer::MultiLevelEdgeBasedGraphView graph_view(
                std::move(node_list), std::move(edge_list), std::move(node_to_offset));
            partitioner::files::readGraph(
                config.GetPath(".osrm.mldgr"), graph_view, graph_connectivity_checksum);

            if (turns_connectivity_checksum != graph_connectivity_checksum)
            {
                throw util::exception(
                    "Connectivity checksum " + std::to_string(graph_connectivity_checksum) +
                    " in " + config.GetPath(".osrm.mldgr").string() +
                    " does not equal to checksum " + std::to_string(turns_connectivity_checksum) +
                    " in " + config.GetPath(".osrm.edges").string());
            }
        }

        // load maneuver overrides
        {
            const auto maneuver_overrides_ptr =
                layout.GetBlockPtr<extractor::StorageManeuverOverride, true>(
                    memory_ptr, DataLayout::MANEUVER_OVERRIDES);
            const auto maneuver_override_node_sequences_ptr = layout.GetBlockPtr<NodeID, true>(
                memory_ptr, DataLayout::MANEUVER_OVERRIDE_NODE_SEQUENCES);

            util::vector_view<extractor::StorageManeuverOverride> maneuver_overrides(
                maneuver_overrides_ptr, layout.GetBlockEntries(DataLayout::MANEUVER_OVERRIDES));
            util::vector_view<NodeID> maneuver_override_node_sequences(
                maneuver_override_node_sequences_ptr,
                layout.GetBlockEntries(DataLayout::MANEUVER_OVERRIDE_NODE_SEQUENCES));

            extractor::files::readManeuverOverrides(config.GetPath(".osrm.maneuver_overrides"),
                                                    maneuver_overrides,
                                                    maneuver_override_node_sequences);
        }
    }
}
}
}
