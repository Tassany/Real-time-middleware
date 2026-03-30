#ifndef ADAPTER_HPP
#define ADAPTER_HPP

template<typename UpstreamComponent, typename DownstreamComponent>
class Adapter{
public:
    typedef typename UpstreamComponent::output_type  upstream_output;
    typedef typename DownstreamComponent::input_type downstream_input;
    Adapter() {}    

    // Define um tipo AdapterFunction, que é um ponteiro para uma função adaptadora
    // Recebe um upstream_output como parametro
    // Retorna downstream_input
    typedef downstream_input (*AdapterFunction)(const upstream_output&);

    Adapter(AdapterFunction func) : func_(func){}

    downstream_input convert(const upstream_output& value) {
        return func_(value);
    }

private:
    AdapterFunction func_;

};

#endif // ADAPTER_HPP