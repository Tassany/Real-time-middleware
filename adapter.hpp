#ifndef ADAPTER_HPP
#define ADAPTER_HPP

/**
 * @file adapter.hpp
 * @brief Type-safe bridge between two Components with different output/input types.
 *
 * When the OutputType of an upstream Component differs from the InputType of a
 * downstream Component, an Adapter wraps a user-supplied conversion function so
 * the pipeline can forward data without exposing raw casts at call sites.
 */

/**
 * @brief Adapts the output of UpstreamComponent to the input of DownstreamComponent.
 *
 * @tparam UpstreamComponent   Component whose output_type is the source of data.
 * @tparam DownstreamComponent Component whose input_type is the destination of data.
 *
 * Example:
 * @code
 *   int double_to_int(const double& v) { return static_cast<int>(v); }
 *   Adapter<ComponentA, ComponentB> adapter(double_to_int);
 *   compB.init_input(adapter.convert(compA.output_));
 * @endcode
 */
template<typename UpstreamComponent, typename DownstreamComponent>
class Adapter {
public:
    typedef typename UpstreamComponent::output_type  upstream_output;
    typedef typename DownstreamComponent::input_type downstream_input;

    /**
     * @brief Pointer to a conversion function: upstream_output -> downstream_input.
     *
     * The function must be pure (no side effects) and must not store a reference to
     * the input value beyond the call, as the upstream buffer may be overwritten.
     */
    typedef downstream_input (*AdapterFunction)(const upstream_output&);

    Adapter() {}

    /** @param func Conversion function called by every convert() invocation. */
    Adapter(AdapterFunction func) : func_(func) {}

    /**
     * @brief Convert an upstream output value to a downstream input value.
     * @param value The value produced by the upstream component's output_.
     * @return The converted value ready to be passed to init_input().
     */
    downstream_input convert(const upstream_output& value) {
        return func_(value);
    }

private:
    AdapterFunction func_;
};

#endif // ADAPTER_HPP