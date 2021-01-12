#pragma once

#include "node.h"
#include "../metawarenn_element.h"

namespace metawarenn {

namespace op {

class InputData : public Node {
  public:
    InputData(std::string n_name, std::vector<int> n_shape, ElementType::element_type n_data_type);
    std::vector<int> shape;
    ElementType::element_type data_type;
};

} //namespace op

} //namespace metawarenn
