#ifndef COMPONENT_HPP
#define COMPONENT_HPP

#include <vector>
#include <string>

// ----------------------------------------------------------------
//  Component classification (paper Section IV)
// ----------------------------------------------------------------
enum class ComponentKind { SOURCE, INTERMEDIATE, SINK };

// ----------------------------------------------------------------
//  ComponentBase — abstract interface for all components
// ----------------------------------------------------------------
class ComponentBase {
public:
    // Called once before the real-time loop; subclasses may override
    // to pre-allocate memory and avoid dynamic allocation during execute().
    virtual void preallocate() {}

    // Invoked by the dispatcher once per subjob.
    virtual void execute() = 0;

    // Returns the component's role in the pipeline.
    // Default is INTERMEDIATE so existing Component<I,O,C> subclasses
    // compile without modification.
    virtual ComponentKind kind() const { return ComponentKind::INTERMEDIATE; }

    virtual ~ComponentBase() = default;
};

// ----------------------------------------------------------------
//  Component<I,O,C> — intermediate component (has both input and output)
// ----------------------------------------------------------------
template<typename InputType, typename OutputType, typename ConfigType>
class Component : public ComponentBase {
public:
    using input_type  = InputType;
    using output_type = OutputType;
    using config_type = ConfigType;

    InputType  input_;
    OutputType output_;

    explicit Component(const ConfigType* config) : config_(config) {}
    ~Component() override = default;

    void init_input (const InputType&  v) { input_  = v; }
    void init_output(const OutputType& v) { output_ = v; }

    ComponentKind kind() const override { return ComponentKind::INTERMEDIATE; }
    void execute() override = 0;

protected:
    const ConfigType* config_;
};

// ----------------------------------------------------------------
//  SourceComponent<O,C> — generates output, consumes no input
// ----------------------------------------------------------------
template<typename OutputType, typename ConfigType>
class SourceComponent : public ComponentBase {
public:
    using output_type = OutputType;
    using config_type = ConfigType;

    OutputType output_;

    explicit SourceComponent(const ConfigType* config) : config_(config) {}
    ~SourceComponent() override = default;

    void init_output(const OutputType& v) { output_ = v; }

    ComponentKind kind() const override { return ComponentKind::SOURCE; }
    void execute() override = 0;

protected:
    const ConfigType* config_;
};

// ----------------------------------------------------------------
//  SinkComponent<I,C> — consumes input, produces no output
// ----------------------------------------------------------------
template<typename InputType, typename ConfigType>
class SinkComponent : public ComponentBase {
public:
    using input_type  = InputType;
    using config_type = ConfigType;

    InputType input_;

    explicit SinkComponent(const ConfigType* config) : config_(config) {}
    ~SinkComponent() override = default;

    void init_input(const InputType& v) { input_ = v; }

    ComponentKind kind() const override { return ComponentKind::SINK; }
    void execute() override = 0;

protected:
    const ConfigType* config_;
};

#endif // COMPONENT_HPP
