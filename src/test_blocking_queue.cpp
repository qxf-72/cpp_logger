#include "BlockingQueue.h"

#include <iostream>
#include <string>
#include <thread>

int main() {
    BlockingQueue<std::string> queue;

    std::thread consumer([&queue] {
        std::string msg;

        while (queue.pop(msg)) {
            std::cout << "consumer got: " << msg << std::endl;
        }

        std::cout << "consumer exit" << std::endl;
        });

    std::thread producer([&queue] {
        for (int i = 0; i < 10; ++i) {
            queue.push("log message " + std::to_string(i));
        }

        queue.close();
        });

    producer.join();
    consumer.join();

    return 0;
}