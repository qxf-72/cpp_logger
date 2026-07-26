#include "Logger.h"

int main() {
  // 调用非内联接口以确认消费者不仅能找到头文件，也能成功链接静态库。
  Logger& logger = Logger::instance();
  return logger.droppedCount() == 0 ? 0 : 1;
}
