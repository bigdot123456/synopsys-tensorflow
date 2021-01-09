#pragma once

#include "metawarenn_optimizer.h"

namespace metawarenn {

namespace optimizer {

class RemoveReshape : public MWNNOptimizer {
  public:
    RemoveReshape();
    RemoveReshape(MWNNGraph* mwnn_graph, MWNNNode mwnn_node);
    void RunPass() override;
};

} //namespace optimizer

} //namespace metawarenn
