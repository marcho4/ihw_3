#include <iostream>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <thread>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <ctime>

volatile sig_atomic_t break_flag = 1;

// Функция для получения текущего времени в формате [HH:MM:SS]
std::string getCurrentTime() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_time = *std::localtime(&time_t);
    
    std::stringstream ss;
    ss << "[" << std::setw(2) << std::setfill('0') << tm_time.tm_hour << ":"
       << std::setw(2) << std::setfill('0') << tm_time.tm_min << ":"
       << std::setw(2) << std::setfill('0') << tm_time.tm_sec << "]";
    return ss.str();
}

void sigint_handler(int sig) {
    break_flag = 0;
    std::cout << std::endl << getCurrentTime() << " Получен сигнал SIGINT. Завершение работы..." << std::endl;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        std::cerr << getCurrentTime() << " Использование: " << argv[0] << " <адрес_сервера> <порт_сервера>" << std::endl;
        return 1;
    }

    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        std::cerr << getCurrentTime() << " ";
        perror("sigaction");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);

    std::string host_address = argv[1];
    int port = std::stoi(argv[2]);

    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        std::cerr << getCurrentTime() << " Ошибка создания сокета" << std::endl;
        return 1;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host_address.c_str(), &server_addr.sin_addr) <= 0) {
        std::cerr << getCurrentTime() << " Неверный адрес или адрес не поддерживается" << std::endl;
        close(socket_fd);
        return 1;
    }

    if (connect(socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << getCurrentTime() << " Соединение не удалось" << std::endl;
        close(socket_fd);
        return 1;
    }
    std::cout << getCurrentTime() << " Подключено к серверу на " << host_address << ":" << port << std::endl;

    // Отправляем идентификационное сообщение
    std::string auth_message = "monitor\n";
    if (send(socket_fd, auth_message.c_str(), auth_message.length(), 0) < 0) {
        std::cerr << getCurrentTime() << " Ошибка отправки идентификационного сообщения" << std::endl;
        close(socket_fd);
        return 1;
    }
    std::cout << getCurrentTime() << " Отправлено идентификационное сообщение: monitor" << std::endl;

    char buffer[4096];
    std::string message_buffer;

    while (break_flag) {
        int n = recv(socket_fd, buffer, sizeof(buffer), 0);
        if (n <= 0) {
            std::cerr << getCurrentTime() << " Соединение закрыто" << std::endl;
            break;
        }
        
        message_buffer.append(buffer, n);
        
        // Пытаемся разделить по \n если они есть
        size_t pos = 0;
        bool found_newline = false;
        while ((pos = message_buffer.find('\n')) != std::string::npos) {
            found_newline = true;
            std::string message = message_buffer.substr(0, pos);
            std::cout << getCurrentTime() << " " << message << std::endl;
            message_buffer.erase(0, pos + 1);
        }
        
        // Если нет символов новой строки или остались данные, выводим их
        if (!found_newline && !message_buffer.empty()) {
            std::cout << getCurrentTime() << " " << message_buffer << std::endl;
            message_buffer.clear();
        }
    }

    std::cout << getCurrentTime() << " Монитор: Отключение..." << std::endl;
    close(socket_fd);
    std::cout << getCurrentTime() << " Монитор: Завершение работы выполнено." << std::endl;
    return 0;
} 