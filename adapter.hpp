#ifndef ADAPTER_HPP
#define ADAPTER_HPP

#include <functional>

// ----------------------------------------------------------------
//  Adapter<Upstream, Downstream>
//
//  Type-safe conversion between an upstream component's output and
//  a downstream component's input (paper Section IV-B).
//
//  Accepts both plain function pointers and lambdas (including those
//  that capture variables), since AdapterFunction is std::function.
// ----------------------------------------------------------------
template<typename UpstreamComponent, typename DownstreamComponent>
class Adapter {
public:
    using upstream_output  = typename UpstreamComponent::output_type;
    using downstream_input = typename DownstreamComponent::input_type;

    using AdapterFunction = std::function<downstream_input(const upstream_output&)>;

    explicit Adapter(AdapterFunction func) : func_(std::move(func)) {}

    downstream_input convert(const upstream_output& value) const {
        return func_(value);
    }

private:
    AdapterFunction func_;
};

#endif // ADAPTER_HPP
