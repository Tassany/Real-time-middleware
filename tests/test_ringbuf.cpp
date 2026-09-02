#include <cassert>
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include "ring_buffer.hpp"

// ----------------------------------------------------------------
// Fase 2 — T1: slot_size() deve ser múltiplo de 64 bytes
// ----------------------------------------------------------------
static void test_ringbuf_cacheline() {
    // Tipos variados; todos devem gerar slots alinhados a cache-line
    static_assert(RingBuffer<char,   4>::slot_size() % CACHE_LINE_SIZE == 0);
    static_assert(RingBuffer<int,    4>::slot_size() % CACHE_LINE_SIZE == 0);
    static_assert(RingBuffer<double, 4>::slot_size() % CACHE_LINE_SIZE == 0);
    static_assert(MultiSupplierRingBuffer<int,    4, 2>::slot_size() % CACHE_LINE_SIZE == 0);
    static_assert(MultiSupplierRingBuffer<double, 4, 3>::slot_size() % CACHE_LINE_SIZE == 0);

    std::cout << "[PASS] test_ringbuf_cacheline\n";
}

// ----------------------------------------------------------------
// Fase 2 — T2: backpressure — write bloqueia com buffer cheio
// ----------------------------------------------------------------
static void test_ringbuf_backpressure() {
    RingBuffer<int, 4> buf;

    // Preenche o buffer (seq 0..3)
    for (size_t i = 0; i < 4; i++) buf.write(i, static_cast<int>(i * 10));

    std::atomic<bool> producer_done{false};

    // Producer tenta escrever seq=4 (mesmo slot que seq=0): deve bloquear
    std::thread producer([&] {
        buf.write(4, 99);
        producer_done.store(true, std::memory_order_release);
    });

    // Verifica que o produtor está bloqueado após 50 ms
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert(!producer_done.load(std::memory_order_acquire));

    // Consumer libera slot 0 → produtor desbloqueia
    buf.release(0);
    producer.join();

    assert(producer_done.load(std::memory_order_acquire));
    assert(buf.read(4) == 99);  // slot 4%4=0 deve ter o novo valor

    std::cout << "[PASS] test_ringbuf_backpressure\n";
}

// ----------------------------------------------------------------
// Fase 2 — T3: MultiSupplierRingBuffer — slot disponível apenas
//              após todos os suppliers escreverem
// ----------------------------------------------------------------
static void test_ringbuf_multifield() {
    MultiSupplierRingBuffer<int, 4, 2> buf;

    // Nenhum supplier escreveu ainda
    assert(!buf.ready(0));

    // Supplier 0 escreve; ainda não está pronto
    buf.write(0, 0, 100);
    assert(!buf.ready(0));

    // Supplier 1 escreve; agora está pronto
    buf.write(0, 1, 200);
    assert(buf.ready(0));

    assert(buf.read(0, 0) == 100);
    assert(buf.read(0, 1) == 200);

    // Release e reutilização do slot (seq=4 usa o mesmo slot que seq=0)
    buf.release(0);
    assert(!buf.ready(4));  // mask foi resetada

    buf.write(4, 0, 300);
    assert(!buf.ready(4));
    buf.write(4, 1, 400);
    assert(buf.ready(4));
    assert(buf.read(4, 0) == 300);
    assert(buf.read(4, 1) == 400);

    std::cout << "[PASS] test_ringbuf_multifield\n";
}

// ----------------------------------------------------------------
// Fase 2 — T4: produtor/consumidor concorrentes — integridade dos dados
// ----------------------------------------------------------------
static void test_ringbuf_concurrent() {
    constexpr size_t JOBS = 100'000;
    RingBuffer<size_t, 16> buf;

    std::atomic<bool> error{false};

    std::thread producer([&] {
        for (size_t i = 0; i < JOBS; i++)
            buf.write(i, i);
    });

    std::thread consumer([&] {
        for (size_t i = 0; i < JOBS; i++) {
            // Spin-wait até que o produtor tenha escrito
            // (em produção o dispatcher notificaria via eventfd)
            while (true) {
                // Lemos otimisticamente; o valor correto chegará
                // quando o produtor terminar de escrever.
                // A garantia de progresso vem do write() do produtor,
                // que termina antes de qualquer write(i+1).
                size_t v = buf.read(i);
                if (v == i) break;
            }
            if (buf.read(i) != i) {
                error.store(true);
            }
            buf.release(i);
        }
    });

    producer.join();
    consumer.join();

    assert(!error.load());
    std::cout << "[PASS] test_ringbuf_concurrent\n";
}

// ----------------------------------------------------------------
// Fase 2 — T5: wraparound — slots reutilizados após N escritas
// ----------------------------------------------------------------
static void test_ringbuf_wraparound() {
    RingBuffer<int, 4> buf;

    // Escreve e libera sequencialmente; slot i%4 deve ter o valor de i
    for (size_t i = 0; i < 8; i++) {
        buf.write(i, static_cast<int>(i));
        assert(buf.read(i) == static_cast<int>(i));
        buf.release(i);
    }

    std::cout << "[PASS] test_ringbuf_wraparound\n";
}

// ----------------------------------------------------------------
// Fase 2 — T6: MultiSupplierRingBuffer com 3 suppliers (fan-in)
// ----------------------------------------------------------------
static void test_ringbuf_fanin_3() {
    MultiSupplierRingBuffer<double, 4, 3> buf;

    buf.write(0, 0, 1.0);
    assert(!buf.ready(0));
    buf.write(0, 2, 3.0);
    assert(!buf.ready(0));
    buf.write(0, 1, 2.0);
    assert(buf.ready(0));

    assert(buf.read(0, 0) == 1.0);
    assert(buf.read(0, 1) == 2.0);
    assert(buf.read(0, 2) == 3.0);

    buf.release(0);

    std::cout << "[PASS] test_ringbuf_fanin_3\n";
}

int main() {
    test_ringbuf_cacheline();
    test_ringbuf_backpressure();
    test_ringbuf_multifield();
    test_ringbuf_concurrent();
    test_ringbuf_wraparound();
    test_ringbuf_fanin_3();
    std::cout << "\nTodos os testes da Fase 2 passaram.\n";
    return 0;
}
