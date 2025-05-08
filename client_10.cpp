#include <queue>
#include <iostream>
#include <unistd.h>
#include <signal.h>
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sstream>
#include <iomanip>


std::queue<std::string> tasks;
int socket_fd;
int break_flag = 1;

void sigint_handler(int sig) {
    break_flag = 0;
    std::cout << "SIGINT получен. Подготовка к завершению программы..." << std::endl;
    close(socket_fd);
    exit(0);
}

enum MessageType {
    REQUEST_CHECK,
    REVIEW_RESULT,
    GET_QUEUE,
};

void send_message(int socket_fd, MessageType message_type, int id_to, int id_from, int result) {
    std::string message_to_send = "";
    if (message_type == REQUEST_CHECK) {
        message_to_send = "check " + std::to_string(id_to) + " " + std::to_string(id_from);
    } else if (message_type == REVIEW_RESULT) {
        message_to_send = "reviewed " + std::to_string(id_to) + " " + std::to_string(id_from) + " " + std::to_string(result);
    } else if (message_type == GET_QUEUE) {
        message_to_send = "queue " + std::to_string(id_to);
    }
    message_to_send += '\n';

    send(socket_fd, message_to_send.c_str(), message_to_send.size(), 0);
}



int main(int argc, char *argv[]) {
    if (argc < 3 || argc > 4) {
        std::cerr << "Использование: " << argv[0] << " <адрес сервера> <порт сервера> [id]" << std::endl;
        return 1;
    }
    bool is_reconnect = false;
    int passed_id = -1;
    if (argc == 4) {
        is_reconnect = true;
        passed_id = atoi(argv[3]);
    }

    srand(time(NULL));
    struct sigaction sa;
    sa.sa_handler = &sigint_handler;
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    std::string host_address = argv[1];
    int port = atoi(argv[2]);

    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        std::cerr << "Ошибка создания сокета" << std::endl;
        return 1;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = inet_addr(host_address.c_str());

    if (connect(socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Ошибка подключения к серверу" << std::endl;
        return 1;
    }
    std::cout << "Соединение установлено" << std::endl;

    // Отправка сообщения о том, что это клиент
    std::string client_message;
    if (is_reconnect) {
        client_message = "client " + std::to_string(passed_id) + "\n";
    } else {
        client_message = "client\n";
    }
    send(socket_fd, client_message.c_str(), client_message.length(), 0);

    char buffer[1024];
    int n = recv(socket_fd, buffer, sizeof(buffer), 0);
    std::string message(buffer, n);
    std::cout << "Сообщение от сервера: \"" << message << "\"" << std::endl;
    std::istringstream iss(message);
    std::string cmd;
    int id;
    iss >> cmd >> id;

    if (cmd == "start") {
        std::cout << "Клиент ID: " << id << " запущен" << std::endl;
    } else if (cmd == "break") {
        break_flag = 0;
        std::cout << "Клиент ID: " << id << " завершен" << std::endl;
        return 0;
    }

    int my_id = id;
    std::cout << "Мой ID: " << my_id << std::endl;

    bool need_new_checker = true;
    int last_checker_id = -1;

    while (break_flag) {
        std::cout << "\nНовая итерация кодирования" << std::endl;
        std::cout << "Идет написание кода..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(rand() % 10 + 1));
        std::cout << "Код написан" << std::endl;

        int checker_id;

        if (need_new_checker) {
            checker_id = rand() % 3;
            while (checker_id == my_id) {
                checker_id = rand() % 3;
            }
            last_checker_id = checker_id;
            need_new_checker = false;
        } else {
            checker_id = last_checker_id;
        }

        std::cout << "Выбираю проверяющего с ID:" << checker_id << std::endl;

        std::string message_to_send = "check " + std::to_string(checker_id) + " " + std::to_string(my_id);
        message_to_send += "\n";
        send(socket_fd, message_to_send.c_str(), message_to_send.size(), 0);

        std::cout << "Запрос на проверку отправлен клиенту ID:" << checker_id << std::endl;

        int last_checker_id = checker_id;
        bool waiting = true;

        std::cout << "Жду результат проверки и обрабатываю задачи..." << std::endl;

        while (waiting && break_flag) {
            std::string message_to_send = "queue " + std::to_string(my_id);
            message_to_send += "\n";
            send(socket_fd, message_to_send.c_str(), message_to_send.size(), 0);

            std::cout << "Запрос к серверу на получение задач из очереди" << std::endl;

            std::string recv_buffer;

            while (waiting) {
                char buffer[1024];
                int n = recv(socket_fd, buffer, sizeof(buffer), 0);
                if (n <= 0) {
                    std::cout << "Сервер отключился или произошла ошибка чтения" << std::endl;
                    break_flag = 0;
                    break;
                }
                recv_buffer.append(buffer, n);
                size_t pos = 0;
                while ((pos = recv_buffer.find('\n')) != std::string::npos) {
                    message = recv_buffer.substr(0, pos);
                    recv_buffer.erase(0, pos + 1);
                    if (message.empty()) {
                        std::cout << "Получено пустое сообщение от сервера" << std::endl;
                        continue;
                    }
                    if (message.substr(0, 5) == "queue") {
                        break;
                    } else {
                        tasks.push(message);
                    }
                }
                
                if (!message.empty() && message.substr(0, 5) == "queue") {
                    break;
                }
                
            }

            std::istringstream iss(message);
            std::string cmd;
            int id_to, id_from;
            iss >> cmd >> id_to >> id_from;
            if (id_to == -1) {
                std::cout << "Жду результата моей проверки..." << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(5)); 
            } else {
                std::cout << "Начинаю проверку кода от клиента ID:" << id_to << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(rand() % 10 + 1));
                std::cout << "Завершена проверка кода от клиента ID:" << id_to << std::endl;
                int result = rand() % 2;
                std::string result_str = (result == 1) ? "ПРИНЯТО" : "ОТКЛОНЕНО";
                std::cout << "Результат проверки: " << result_str << std::endl;
                send_message(socket_fd, REVIEW_RESULT, id_to, my_id, result);
            }
            
            if (!tasks.empty()) {
                std::cout << "Обрабатываю задачи, полученные во время ожидания (" << tasks.size() << " шт.)" << std::endl;
            }
            
            while (!tasks.empty()) {
                std::string message = tasks.front();
                tasks.pop();
                if (message.substr(0, 5) == "check") {
                    std::istringstream iss(message);
                    std::string cmd;
                    int id_to, id_from;
                    iss >> cmd >> id_to >> id_from;
                    std::cout << "Получен запрос на проверку кода от клиента ID:" << id_from << std::endl;
                    std::this_thread::sleep_for(std::chrono::seconds(rand() % 10 + 1));
                    std::cout << "Завершена проверка кода от клиента ID:" << id_from << std::endl;
                    int result = rand() % 2;
                    std::string result_str = (result == 1) ? "ПРИНЯТО" : "ОТКЛОНЕНО";
                    std::cout << "Результат проверки: " << result_str << std::endl;
                    send_message(socket_fd, REVIEW_RESULT, id_from, my_id, result);
                    std::cout << "Результат проверки отправлен на сервер" << std::endl;

                } else if (message.substr(0, 8) == "reviewed") {
                    std::istringstream iss(message);
                    std::string cmd;
                    int id_to, id_from, result;
                    iss >> cmd >> id_to >> id_from >> result;
                    std::cout << "Получен результат проверки моего кода от клиента ID:" << id_from << std::endl;
                    if (result == 0) {
                        std::cout << "Проверка не пройдена! Нужно исправить код" << std::endl;
                        need_new_checker = false;
                        waiting = false;
                        std::cout << "Начинаю переписывать код..." << std::endl;
                        break;
                    } else {
                        std::cout << "Проверка пройдена успешно!" << std::endl;
                        waiting = false;
                        need_new_checker = true;
                        break;
                    }
                }
            }
        }
    }

    close(socket_fd);
    std::cout << "Клиент ID:" << my_id << " завершает работу...\n";
    return 0;
}