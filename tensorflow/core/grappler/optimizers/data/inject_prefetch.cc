/* Copyright 2021 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/core/grappler/optimizers/data/inject_prefetch.h"

#include "tensorflow/core/framework/model.h"
#include "tensorflow/core/framework/node_def.pb.h"
#include "tensorflow/core/grappler/clusters/cluster.h"
#include "tensorflow/core/grappler/grappler_item.h"
#include "tensorflow/core/grappler/mutable_graph_view.h"
#include "tensorflow/core/grappler/op_types.h"
#include "tensorflow/core/grappler/optimizers/custom_graph_optimizer_registry.h"
#include "tensorflow/core/grappler/optimizers/data/graph_utils.h"
#include "tensorflow/core/grappler/utils.h"
#include "tensorflow/core/platform/protobuf.h"

namespace tensorflow {
namespace grappler {
namespace {

constexpr char kPrefetchDataset[] = "PrefetchDataset";

}  // namespace

Status InjectPrefetch::OptimizeAndCollectStats(Cluster* cluster,
                                               const GrapplerItem& item,
                                               GraphDef* output,
                                               OptimizationStats* stats) {
  *output = item.graph;
  if (!autotune_) {
    VLOG(1) << "The optimization inject_prefetch is not applied if autotune is "
               "off.";
    return Status::OK();
  }
  MutableGraphView graph(output);

  // If the GrapplerItem is derived from a FunctionDef, we don't optimize it.
  if (graph_utils::IsItemDerivedFromFunctionDef(item, graph))
    return Status::OK();

  if (item.fetch.size() != 1) {
    return errors::InvalidArgument(
        "Expected only one fetch node but there were ", item.fetch.size(), ": ",
        absl::StrJoin(item.fetch, ", "));
  }

  NodeDef* sink_node = graph.GetNode(item.fetch.at(0));
  NodeDef* last_node = graph_utils::GetInputNode(*sink_node, graph);

  if (last_node->op() == kPrefetchDataset) {
    VLOG(1) << "The optimization inject_prefetch is not applied since the last "
               "dataset is already prefetched.";
    return Status::OK();
  }

  // Insert `prefetch(AUTOTUNE)` after the last node.
  NodeDef prefetch_node;
  graph_utils::SetUniqueGraphNodeName(
      strings::StrCat("inject/prefetch_", last_node->name()), graph.graph(),
      &prefetch_node);
  prefetch_node.set_op(kPrefetchDataset);
  // `input_dataset` input
  *prefetch_node.mutable_input()->Add() = last_node->name();
  // `buffer_size` input
  NodeDef* autotune_value =
      graph_utils::AddScalarConstNode(data::model::kAutotune, &graph);
  *prefetch_node.mutable_input()->Add() = autotune_value->name();

  // Set `output_types` and `output_shapes` attributes by copying the relevant
  // attrs from the input node. If we fail to set the attributes, we abort the
  // rewrite.
  if (!graph_utils::CopyShapesAndTypesAttrs(*last_node, &prefetch_node))
    return Status::OK();

  auto* added_node = graph.AddNode(std::move(prefetch_node));
  TF_RETURN_IF_ERROR(
      graph.UpdateFanouts(last_node->name(), added_node->name()));

  stats->num_changes++;
  return Status::OK();
}

REGISTER_GRAPH_OPTIMIZER_AS(InjectPrefetch, "inject_prefetch");

}  // namespace grappler
}  // namespace tensorflow
