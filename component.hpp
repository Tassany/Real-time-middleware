#ifndef COMPONENT_HPP
#define COMPONENT_HPP

#include <vector>
#include <string>
#include <atomic>
#include "ring_buffer.hpp"

// ================================================================
//  ComponentPort<T, N>
//
//  Lock-free ring-buffer backed I/O port for a Component.
//  Each port owns a RingBuffer<T,N> plus two sequence counters
//  so producer and consumer can advance independently.
//
//  Default capacity N=8 is sufficient for pipelines where the
//  downstream always consumes before the next job arrives.
//  Increase N if the pipeline depth (concurrent jobs) is higher.
// ================================================================

template<typename T, size_t N = 8>
struct ComponentPort {
    RingBuffer<T, N>    buffer;
    std::atomic<size_t> producer_seq{0};
    std::atomic<size_t> consumer_seq{0};

    // Push a value from an upstream component.
    // Returns false if the port is full (backpressure).
    bool push(const T& value) {
        return buffer.write(
            producer_seq.fetch_add(1, std::memory_order_relaxed), value);
    }

    // Peek at the next unconsumed value (reference valid until consume()).
    T& peek() {
        return buffer.read(consumer_seq.load(std::memory_order_acquire));
    }

    // Advance the consumer counter and release the slot.
    void consume() {
        size_t s = consumer_seq.fetch_add(1, std::memory_order_relaxed);
        buffer.release(s);
    }

    // True if at least one value is waiting to be consumed.
    bool has_data() const {
        return buffer.has_data(consumer_seq.load(std::memory_order_acquire));
    }
};


// ================================================================
//  ComponentBase
//
//  Non-template base class allowing heterogeneous storage of
//  Components. The DAG and Dispatcher hold ComponentBase* so
//  they can trigger execution without knowing concrete I/O types.
// ================================================================

class ComponentBase {
public:
    virtual void execute() = 0;

    // Returns true when the component's input_port has unconsumed data.
    // Overridden by Component<> to delegate to input_port.has_data().
    // Used by the Demultiplexer Step 5 drain to keep executing while
    // there are pending inputs (§V-C of the paper).
    virtual bool has_pending_input() const { return false; }

    virtual ~ComponentBase() = default;
};


// ================================================================
//  Component<InputType, OutputType, ConfigType>
//
//  Typed computational component. Subclasses implement execute()
//  and may read from input_ / input_port and write to
//  output_ / output_port.
//
//  Both plain members (input_, output_) and ring-buffer ports
//  (input_port, output_port) are available. Use plain members for
//  simple intra-thread pipelines; use ports for lock-free
//  inter-thread communication without marshaling (§V-B).
// ================================================================

template<typename InputType, typename OutputType, typename ConfigType>
class Component : public ComponentBase {
public:
    typedef InputType  input_type;
    typedef OutputType output_type;
    typedef ConfigType config_type;

    // Plain I/O members — simple, backward-compatible path.
    InputType  input_;
    OutputType output_;

    // Ring-buffer ports — lock-free inter-thread path (§V-B).
    ComponentPort<InputType>  input_port;
    ComponentPort<OutputType> output_port;

    explicit Component(const ConfigType* config) : config_(config) {}
    ~Component() override {}

    void init_input(const InputType& input)    { input_  = input; }
    void init_output(const OutputType& output) { output_ = output; }

    virtual void execute() override = 0;

    // Delegates to input_port so the Demultiplexer Step 5 drain
    // can keep executing while unconsumed data remains.
    bool has_pending_input() const override {
        return input_port.has_data();
    }

protected:
    const ConfigType* config_;
};

#endif // COMPONENT_HPP
