/*
 * This file belongs to the Galois project, a C++ library for exploiting parallelism.
 * The code is being released under the terms of the 3-Clause BSD License (a
 * copy is located in LICENSE.txt at the top-level directory).
 *
 * Copyright (C) 2018, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 */

#include "galois/Galois.h"
#include "galois/gstl.h"
#include "galois/Reduction.h"
#include "galois/Timer.h"
#include "galois/Timer.h"
#include "galois/graphs/LCGraph.h"
#include "galois/graphs/TypeTraits.h"
#include "llvm/Support/CommandLine.h"
#include "galois/AtomicHelpers.h"

#include "Lonestar/BoilerPlate.h"

#include <iostream>
#include <deque>
#include <type_traits>

namespace cll = llvm::cl;

static const char* name = "Louvain Clustering";

static const char* desc =
    "Cluster nodes of the graph using Louvain Clustering";

static const char* url = "louvain_clustering";

enum Algo {
  naive
};


static cll::opt<std::string>
    filename(cll::Positional, cll::desc("<input graph>"), cll::Required);

static cll::opt<Algo> algo(
    "algo", cll::desc("Choose an algorithm:"),
    cll::values(clEnumValN(Algo::naive, "Naive", "Naive Implementation"),
                             clEnumValEnd),
    cll::init(Algo::naive));

static cll::opt<bool> enable_VF("enable_VF",
  cll::desc("Flag to enable vertex following optimization."),
  cll::init(false));

static cll::opt<double> c_threshold("c_threshold",
  cll::desc("Threshold for modularity gain"),
  cll::init(0.01));




/*
 * Typedefs
 */

constexpr static const uint64_t INF_VAL = std::numeric_limits<uint64_t>::max() / 2 - 1;

constexpr galois::MethodFlag flag_no_lock = galois::MethodFlag::UNPROTECTED;
constexpr galois::MethodFlag flag_read_lock = galois::MethodFlag::READ;
constexpr galois::MethodFlag flag_write_lock = galois::MethodFlag::WRITE;

typedef galois::LargeArray<uint64_t> largeArray;

//Maintain community information
struct Comm {
  std::atomic<uint64_t> size;
  std::atomic<uint64_t> degree_wt;
};

typedef galois::LargeArray<Comm> CommArray;

//Graph Node information
struct Node{
  uint64_t prev_comm_ass;
  uint64_t curr_comm_ass;
  uint64_t degree_wt;
  uint64_t cluster_wt_internal;
};

using Graph =
    galois::graphs::LC_CSR_Graph<Node, uint32_t>::with_no_lockable<false>::type::with_numa_alloc<true>::type;

using GNode = Graph::GraphNode;


uint64_t vertexFollowing(Graph& graph, largeArray& clusters){

  //Initialize each node to its own cluster
  galois::do_all(galois::iterate(graph),
                 [&graph](GNode n) { graph.getData(n).curr_comm_ass = n; });

  //Remove isolated and degree-one nodes
  galois::GAccumulator<uint64_t> isolatedNodes;
  galois::do_all(galois::iterate(graph),
                  [&](GNode n) {
                    uint64_t degree = std::distance(graph.edge_begin(n, galois::MethodFlag::UNPROTECTED),
                                                     graph.edge_end(n, galois::MethodFlag::UNPROTECTED));
                    if(degree == 0) {
                      isolatedNodes += 1;
                      clusters[n] = -1;
                    } else {
                      if(degree == 1) {
                        //Check if the destination has degree greater than one
                        auto dst = graph.getEdgeDst(graph.edge_end(n, galois::MethodFlag::UNPROTECTED));
                        uint64_t dst_degree = std::distance(graph.edge_begin(dst, galois::MethodFlag::UNPROTECTED),
                                                         graph.edge_end(dst, galois::MethodFlag::UNPROTECTED));
                        if((dst_degree > 1 || (n > dst))){
                          isolatedNodes += 1;
                          clusters[n] = graph.getData(dst).curr_comm_ass;
                        }
                      }
                    }
                  });
  //The number of isolated nodes that can be removed
  return isolatedNodes.reduce();
}



void sumVertexDegreeWeight(Graph& graph, CommArray& c_info) {
  galois::do_all(galois::iterate(graph),
                [&](GNode n) {
                  uint64_t total_weight = 0;
                  auto &n_data = graph.getData(n);
                  for(auto ii = graph.edge_begin(n); ii != graph.edge_end(n); ++ii){
                     total_weight += graph.getEdgeData(ii, flag_no_lock);
                  }
                  n_data.degree_wt = total_weight;
                  c_info[n].degree_wt = total_weight;
                  c_info[n].size = 1;
                });
}

double calConstantForSecondTerm(Graph& graph){
  galois::GAccumulator<uint64_t> local_weight;
  galois::do_all(galois::iterate(graph),
                [&graph, &local_weight](GNode n){
                  local_weight += graph.getData(n).degree_wt;
                });
  /* This is twice since graph is symmetric */
  uint64_t total_edge_weight_twice = local_weight.reduce();
  return 1/(double)total_edge_weight_twice;
}


uint64_t maxModularity(std::map<uint64_t, uint64_t> &cluster_local_map, std::vector<uint64_t> &counter, uint64_t self_loop_wt,
                       CommArray &c_info, uint64_t degree_wt, uint64_t sc, double constant) {

  uint64_t max_index = sc; // Assign the intial value as self community
  double cur_gain = 0;
  double max_gain = 0;
  double eix = counter[0] - self_loop_wt;
  double ax = c_info[sc].degree_wt - degree_wt;
  double eiy = 0;
  double ay = 0;

  auto stored_already = cluster_local_map.begin();
  do {
    if(sc != stored_already->first) {
      ay = c_info[stored_already->first].degree_wt; // Degree wt of cluster y
      eiy = counter[stored_already->second]; // Total edges incident on cluster y
      //cur_gain = 2 * (eiy - eix) - 2 * degree_wt * (ay - ax) * constant;
      //From the paper: Verbatim
      cur_gain = 2 * constant * (eiy - eix) + 2 * degree_wt * (ax - ay) * constant * constant;

      if( (cur_gain > max_gain) ||  ((cur_gain == max_gain) && (cur_gain != 0) && (stored_already->first < max_index))) {
        max_gain = cur_gain;
        //galois::gPrint("Gain : ", max_gain, "\n");
        max_index = stored_already->first;
      }
    }
    stored_already++; // Explore next cluster
  } while (stored_already != cluster_local_map.end());

  if(c_info[max_index].size == 1 && c_info[sc].size == 1 && max_index > sc ) {
    max_index = sc;
  }

  return max_index;
}

uint64_t renumberClustersContiguously(Graph &graph) {

}
double algoLouvainWithLocking(Graph &graph, largeArray& clusters, double lower, double threshold) {
  galois::gPrint("Inside algoLouvainWithLocking\n");
  CommArray c_info; // Community info
  CommArray c_update; // Used for updating community

  /* Variables needed for Modularity calculation */
  uint64_t total_edge_wt_twice;
  double constant_for_second_term;
  double prev_mod = lower;
  double curr_mod = -1;
  double threshold_mod = threshold;
  uint32_t num_iter = 0;
  largeArray cluster_wt_internal;


  /*** Initialization ***/
  c_info.allocateBlocked(graph.size());
  c_update.allocateBlocked(graph.size());
  cluster_wt_internal.allocateBlocked(graph.size()); 

  /* Initialization each node to its own cluster */
  galois::do_all(galois::iterate(graph),
                [&graph](GNode n) {
                  graph.getData(n).curr_comm_ass = n;
                  graph.getData(n).prev_comm_ass = n;
                  });

  /* Calculate the weighted degree sum for each vertex */
  sumVertexDegreeWeight(graph, c_info);
  galois::gPrint("c_info[5] : ", c_info[5].degree_wt.load(), "\n");

  /* Compute the total weight (2m) and 1/2m terms */
  constant_for_second_term = calConstantForSecondTerm(graph);
  galois::gPrint("constant_for_second_term : ", constant_for_second_term, "\n");

  galois::gPrint("========================================================================================================\n");
  galois::gPrint("Itr      Explore_xx            A_x2           Curr-Mod         Time-1(s)       Time-2(s)        T/Itr(s)\n");
  galois::gPrint("========================================================================================================\n");
  while(true) {
    num_iter++;

  galois::do_all(galois::iterate(graph),
                [&](GNode n) {
                  cluster_wt_internal[n] = 0;
                  c_update[n].degree_wt = 0;
                  c_update[n].size = 0;
                  });

  galois::for_each(galois::iterate(graph),
                [&](GNode n, auto& ctx) {
                    auto& n_data = graph.getData(n, flag_write_lock);
                    uint64_t degree = std::distance(graph.edge_begin(n, galois::MethodFlag::UNPROTECTED),
                                                     graph.edge_end(n, galois::MethodFlag::UNPROTECTED));
                    //TODO: Can we make it infinity??
                    uint64_t local_target = -1;
                    std::map<uint64_t, uint64_t> cluster_local_map; // Map each neighbor's cluster to local number: Community --> Index
                    std::vector<uint64_t> counter; //Number of edges to each unique cluster
                    uint64_t num_unique_clusters = 1;
                    uint64_t self_loop_wt = 0;

                    if(degree > 0){
                      cluster_local_map[graph.getData(n).curr_comm_ass] = 0; // Add n's current cluster
                      counter.push_back(0); //Initialize the counter to zero (no edges incident yet)

                      //TODO: Make this cautious operator; Find better way
                      //Grab lock on all the neighbors before making any changes
                      for(auto ii = graph.edge_begin(n); ii != graph.edge_end(n); ++ii) {
                        graph.getData(graph.getEdgeDst(ii), flag_write_lock);
                      }

                      //Assuming we have grabbed lock on all the neighbors
                      for(auto ii = graph.edge_begin(n); ii != graph.edge_end(n); ++ii) {
                        GNode dst = graph.getEdgeDst(ii);
                        auto edge_wt = graph.getEdgeData(ii, flag_no_lock); // Self loop weights is recorded
                        if(dst == n){
                          self_loop_wt += edge_wt; // Self loop weights is recorded
                        }
                        auto stored_already = cluster_local_map.find(graph.getData(dst).curr_comm_ass); // Check if it already exists
                        if(stored_already != cluster_local_map.end()) {
                          counter[stored_already->second] += edge_wt;
                        } else {
                         cluster_local_map[graph.getData(dst).curr_comm_ass] = num_unique_clusters;
                         counter.push_back(edge_wt);
                         num_unique_clusters++;
                        }
                      } // End edge loop

                    // Find the max gain in modularity
                    local_target = maxModularity(cluster_local_map, counter, self_loop_wt, c_info, n_data.degree_wt, n_data.curr_comm_ass, constant_for_second_term);

                    } else {
                      local_target = -1;
                    }

                    /* Update cluster info */
                    if(local_target != n_data.curr_comm_ass && local_target != -1) {

                      galois::atomicAdd(c_info[local_target].degree_wt, n_data.degree_wt);
                      galois::atomicAdd(c_info[local_target].size, (uint64_t)1);
                      galois::atomicSubtract(c_info[n_data.curr_comm_ass].degree_wt, n_data.degree_wt);
                      galois::atomicSubtract(c_info[n_data.curr_comm_ass].size, (uint64_t)1);

                      //galois::atomicAdd(c_update[local_target].degree_wt, n_data.degree_wt);
                      //galois::atomicAdd(c_update[local_target].size, (uint64_t)1);
                      //galois::atomicSubtract(c_update[n_data.curr_comm_ass].degree_wt, n_data.degree_wt);
                      //galois::atomicSubtract(c_update[n_data.curr_comm_ass].size, (uint64_t)1);
                    }

                    /* Set the new cluster id */
                    n_data.curr_comm_ass = local_target;

                    /* Update c info: Asynchronously TODO: Is this correct? */
                    //galois::atomicAdd(c_info[n].size,  c_update[n].size.load());
                    //galois::atomicAdd(c_info[n].degree_wt, c_info[n].degree_wt.load());

                }, galois::loopname("louvain algo: Phase 1")//,
                   //galois::no_pushes()
                );

     /* Calculate the overall modularity */
    double e_xx = 0;
    double a2_x = 0;

    galois::do_all(galois::iterate(graph),
                  [&](GNode n) {
                    cluster_wt_internal[n] = 0;
                    //galois::gPrint("kk: ", n, " : ", graph.getData(n).curr_comm_ass, "\n");
                  });

    galois::GAccumulator<uint64_t> same_nodes, same_nodes2, num_edges;
    galois::do_all(galois::iterate(graph),
                  [&](GNode n) {
                    auto n_data = graph.getData(n);
                    for(auto ii = graph.edge_begin(n); ii != graph.edge_end(n); ++ii) {
                      num_edges += 1;
                      if(graph.getData(graph.getEdgeDst(ii)).curr_comm_ass == n_data.curr_comm_ass) {
                        cluster_wt_internal[n] += graph.getEdgeData(ii);
                        same_nodes += 1;
                      }
                    }
                  });

    galois::gPrint("same_nodes : ", same_nodes.reduce(), "\n");
    galois::gPrint("edges : ", num_edges.reduce(), "\n");
    galois::do_all(galois::iterate(graph),
                  [&](GNode n) {
                    e_xx += cluster_wt_internal[n];
                    a2_x += (c_info[n].degree_wt) * (c_info[n].degree_wt);
                  });


    //galois::gPrint("e_xx : ", e_xx, " ,constant_for_second_term : ", constant_for_second_term, " a2_x : ", a2_x, "\n");
    curr_mod = e_xx * (double)constant_for_second_term - a2_x * (double)constant_for_second_term * (double)constant_for_second_term;
    galois::gPrint("Mod : ", curr_mod, "\n");
    galois::gPrint("prev Mod : ", prev_mod, "\n");
    galois::gPrint(num_iter, "        ", e_xx, "        ", a2_x, "        ", curr_mod, "\n");

    if((curr_mod - prev_mod) < threshold_mod){
      galois::gPrint("Modularity gain: ", (curr_mod - prev_mod) , " < ", threshold_mod, " \n");
      break;
    }

    prev_mod = curr_mod;

  //TODO: remove this
  //break;
  }// End while
  return prev_mod;
}


void runMultiPhaseLouvainAlgorithm(Graph& graph, largeArray& clusters_orig, uint64_t min_graph_size, double c_threshold) {

  double prev_mod = -1; //Previous modularity
  double curr_mod = -1; //Current modularity
  uint32_t phase = 1;

  /*
   *Initialize node cluster id locally.
   */
  largeArray clusters_local;
  clusters_local.allocateBlocked(graph.size());
  galois::do_all(galois::iterate(graph),
                [&](GNode n){
                  clusters_local[n] = INF_VAL;
                });

  while(true){
    galois::gPrint("Starting Phase : ", phase, "\n");
    prev_mod = curr_mod;

    //TODO: add to the if conditional
    if(graph.size() > min_graph_size){
      curr_mod = algoLouvainWithLocking(graph, clusters_local, curr_mod, c_threshold);
    }

    //TODO:remove this
    break;

  }

}

int main(int argc, char** argv) {
  galois::SharedMemSys G;
  LonestarStart(argc, argv, name, desc, url);

  Graph graph;
  GNode source, report;

  std::cout << "Reading from file: " << filename << std::endl;
  std::cout << "[WARNING:] Make sure " << filename << " is symmetric graph without duplicate edges" << std::endl;
  galois::graphs::readGraph(graph, filename);
  std::cout << "Read " << graph.size() << " nodes, " << graph.sizeEdges()
            << " edges" << std::endl;

  /*
   * Vertex following optimization
   */
  if (enable_VF){
    largeArray clusters; // Cluster assignment
    clusters.allocateBlocked(graph.size());

    uint64_t num_nodes_to_fix = vertexFollowing(graph, clusters); // Find nodes that follow other nodes
    galois::gPrint("Isolated nodes : ", num_nodes_to_fix, "\n");

    //TODO:
    //Build new graph to remove the isolated nodes
  }

  largeArray clusters_orig;
  clusters_orig.allocateBlocked(graph.size());

  /*
   *Initialize node cluster id.
   */
  galois::do_all(galois::iterate(graph),
                [&](GNode n){
                  graph.getData(n).curr_comm_ass = INF_VAL;
                  clusters_orig[n] = INF_VAL;
                });

  //double c_threshold = 0.01;
  uint64_t min_graph_size = 100;
  runMultiPhaseLouvainAlgorithm(graph, clusters_orig, min_graph_size, c_threshold);
  return 0;
}