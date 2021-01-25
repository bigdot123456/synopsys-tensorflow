#pragma once

#include <iostream>
#include "../metawarenn_graph.h"

namespace metawarenn {

namespace optimizer {

class MWNNOptimizer {
  public:
    MWNNOptimizer();
    virtual ~MWNNOptimizer() {}
    void set_name(const std::string& name) { pass_name = name; }
    std::string get_name() { return pass_name; }
    virtual void RunPass() = 0;
  private:
    std::string pass_name;
  protected:
    MWNNGraph *graph;
    MWNNNode node;
    MWNNTensor tensor;
    MWNNValueInfo value_info;
    std::set<std::string> producers;
    std::set<std::string> consumers;
};
} //namespace optimizer

} //namespace metawarenn
