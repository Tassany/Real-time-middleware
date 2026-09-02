#include <cassert>
#include <iostream>
#include <vector>
#include <string>
#include <type_traits>
#include "component.hpp"
#include "adapter.hpp"

// ---------------------------------------------------------------
//  Helpers: detectar presença de input_type / output_type
// ---------------------------------------------------------------
template<typename T, typename = void>
struct has_input_type : std::false_type {};
template<typename T>
struct has_input_type<T, std::void_t<typename T::input_type>> : std::true_type {};

template<typename T, typename = void>
struct has_output_type : std::false_type {};
template<typename T>
struct has_output_type<T, std::void_t<typename T::output_type>> : std::true_type {};

// ---------------------------------------------------------------
//  Configuração mínima reutilizada nos testes
// ---------------------------------------------------------------
struct DummyConfig { int value = 0; };

// ---------------------------------------------------------------
//  Componentes concretos para teste
// ---------------------------------------------------------------
class ConcreteIntermediate : public Component<int, double, DummyConfig> {
public:
    explicit ConcreteIntermediate(const DummyConfig* c) : Component(c) {}
    void execute() override { output_ = input_ * 2.0; }
};

class ConcreteSource : public SourceComponent<double, DummyConfig> {
public:
    explicit ConcreteSource(const DummyConfig* c) : SourceComponent(c) {}
    void execute() override { output_ = 42.0; }
};

class ConcreteSink : public SinkComponent<int, DummyConfig> {
public:
    explicit ConcreteSink(const DummyConfig* c) : SinkComponent(c) {}
    void execute() override { /* consume input_ */ }
};

// Componente que registra a ordem das chamadas
class MockComponent : public Component<int, int, DummyConfig> {
public:
    std::vector<std::string> call_log;

    explicit MockComponent(const DummyConfig* c) : Component(c) {}
    void preallocate() override { call_log.push_back("preallocate"); }
    void execute()     override { call_log.push_back("execute"); }
};

// ---------------------------------------------------------------
//  T1: kind() retorna o valor correto para cada categoria
// ---------------------------------------------------------------
static void test_component_kinds() {
    DummyConfig cfg;

    ConcreteIntermediate intermediate(&cfg);
    ConcreteSource       source(&cfg);
    ConcreteSink         sink(&cfg);

    assert(intermediate.kind() == ComponentKind::INTERMEDIATE);
    assert(source.kind()       == ComponentKind::SOURCE);
    assert(sink.kind()         == ComponentKind::SINK);

    // Polimorfismo via ponteiro base
    ComponentBase* b = &intermediate;
    assert(b->kind() == ComponentKind::INTERMEDIATE);
    b = &source;
    assert(b->kind() == ComponentKind::SOURCE);
    b = &sink;
    assert(b->kind() == ComponentKind::SINK);

    std::cout << "[PASS] test_component_kinds\n";
}

// ---------------------------------------------------------------
//  T2: SourceComponent não tem input_type; SinkComponent não tem output_type
// ---------------------------------------------------------------
static void test_component_type_constraints() {
    // Component (intermediate) tem ambos
    static_assert(has_input_type<ConcreteIntermediate>::value);
    static_assert(has_output_type<ConcreteIntermediate>::value);

    // SourceComponent tem apenas output_type
    static_assert(!has_input_type<ConcreteSource>::value);
    static_assert(has_output_type<ConcreteSource>::value);

    // SinkComponent tem apenas input_type
    static_assert(has_input_type<ConcreteSink>::value);
    static_assert(!has_output_type<ConcreteSink>::value);

    std::cout << "[PASS] test_component_type_constraints\n";
}

// ---------------------------------------------------------------
//  T3: Adapter com ponteiro de função (retrocompatibilidade)
// ---------------------------------------------------------------
static double times_three(const int& v) { return v * 3.0; }

static void test_adapter_function_ptr() {
    DummyConfig cfg;
    ConcreteIntermediate upstream(&cfg);   // output_type = double ... wait
    // upstream: int → double
    // Let's make a downstream that takes double → int
    // We need Component<double, int, DummyConfig>
    struct DownComp : Component<double, int, DummyConfig> {
        explicit DownComp(const DummyConfig* c) : Component(c) {}
        void execute() override { output_ = static_cast<int>(input_); }
    };

    // Adapter: ConcreteIntermediate(int→double) → DownComp(double→int)
    // upstream_output = double, downstream_input = double
    // The adapter converts upstream output (double) to downstream input (double)
    // No conversion needed here — same type. Use identity.
    DummyConfig cfg2;
    DownComp downstream(&cfg2);

    Adapter<ConcreteIntermediate, DownComp> adapter(
        [](const double& v) -> double { return v; }
    );

    upstream.init_input(5);
    upstream.init_output(0.0);
    upstream.execute();  // output_ = 5 * 2.0 = 10.0

    double result = adapter.convert(upstream.output_);
    assert(result == 10.0);

    // Also test with a plain function pointer
    struct Up2 : Component<int, int, DummyConfig> {
        explicit Up2(const DummyConfig* c) : Component(c) {}
        void execute() override { output_ = input_; }
    };
    struct Down2 : Component<int, double, DummyConfig> {
        explicit Down2(const DummyConfig* c) : Component(c) {}
        void execute() override { output_ = input_; }
    };
    DummyConfig cfg3;
    Adapter<Up2, Down2> adapter2(times_three);  // function pointer implicit → std::function

    Up2 up2(&cfg3);
    up2.init_input(4);
    up2.init_output(0);
    up2.execute();  // output_ = 4

    assert(adapter2.convert(up2.output_) == 12.0);

    std::cout << "[PASS] test_adapter_function_ptr\n";
}

// ---------------------------------------------------------------
//  T4: Adapter com lambda capturando variável (closure)
// ---------------------------------------------------------------
static void test_adapter_lambda() {
    struct Up : Component<int, int, DummyConfig> {
        explicit Up(const DummyConfig* c) : Component(c) {}
        void execute() override { output_ = input_; }
    };
    struct Down : Component<int, double, DummyConfig> {
        explicit Down(const DummyConfig* c) : Component(c) {}
        void execute() override {}
    };

    double factor = 2.5;
    Adapter<Up, Down> adapter([factor](const int& v) -> double {
        return v * factor;
    });

    DummyConfig cfg;
    Up up(&cfg);
    up.init_input(4);
    up.init_output(0);
    up.execute();  // output_ = 4

    assert(adapter.convert(up.output_) == 10.0);

    // Mudar o factor capturado em uma nova lambda
    factor = 3.0;
    Adapter<Up, Down> adapter2([factor](const int& v) -> double {
        return v * factor;
    });
    assert(adapter2.convert(up.output_) == 12.0);

    std::cout << "[PASS] test_adapter_lambda\n";
}

// ---------------------------------------------------------------
//  T5: preallocate() é chamado antes de execute()
// ---------------------------------------------------------------
static void test_preallocate_order() {
    DummyConfig cfg;
    MockComponent comp(&cfg);

    // Simula o que o dispatcher fará: preallocate uma vez, execute N vezes
    comp.preallocate();
    comp.execute();
    comp.execute();

    assert(comp.call_log.size() == 3);
    assert(comp.call_log[0] == "preallocate");
    assert(comp.call_log[1] == "execute");
    assert(comp.call_log[2] == "execute");

    std::cout << "[PASS] test_preallocate_order\n";
}

// ---------------------------------------------------------------
//  T6: preallocate() default é no-op (não quebra subclasses existentes)
// ---------------------------------------------------------------
static void test_preallocate_default_noop() {
    DummyConfig cfg;
    ConcreteIntermediate comp(&cfg);

    // Não deve lançar exceção nem ter efeito colateral
    comp.preallocate();
    comp.init_input(3);
    comp.init_output(0.0);
    comp.execute();
    assert(comp.output_ == 6.0);

    std::cout << "[PASS] test_preallocate_default_noop\n";
}

// ---------------------------------------------------------------
//  T7: SourceComponent e SinkComponent executam corretamente
// ---------------------------------------------------------------
static void test_source_sink_execute() {
    DummyConfig cfg;

    ConcreteSource src(&cfg);
    src.init_output(0.0);
    src.execute();
    assert(src.output_ == 42.0);

    ConcreteSink sink(&cfg);
    sink.init_input(99);
    sink.execute();  // deve apenas consumir, sem crash

    std::cout << "[PASS] test_source_sink_execute\n";
}

int main() {
    test_component_kinds();
    test_component_type_constraints();
    test_adapter_function_ptr();
    test_adapter_lambda();
    test_preallocate_order();
    test_preallocate_default_noop();
    test_source_sink_execute();
    std::cout << "\nTodos os testes da Fase 3 passaram.\n";
    return 0;
}
