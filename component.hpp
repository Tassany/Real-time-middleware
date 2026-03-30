#ifndef COMPONENT_HPP
#define COMPONENT_HPP

#include <vector>
#include <string>

template<typename InputType, typename OutputType, typename ConfigType>
class Component {
public:
    typedef InputType  input_type;
    typedef OutputType output_type;
    typedef ConfigType config_type;

    InputType  input_;
    OutputType output_;

    
    Component(const ConfigType* config) : config_(config) {}
    ~Component() {}

    
    void init_input(const InputType& input)   { input_  = input; }
    void init_output(const OutputType& output) { output_ = output; }

    
    virtual void execute() = 0;

protected:
    const ConfigType* config_;
};

#endif // COMPONENT_HPP