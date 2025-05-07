#include <queue>
#include <iostream>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <thread>
#include <sstream>
#include <map>
#include <atomic>
#include <string>
#include <iomanip>
#include <mutex>
#include <cstring>
#include <vector>

int break_flag = 1;
std::vector<int> monitor_socket_fds;
std::mutex monitor_socket_mutex;

// Класс чтобы логировать в консоль и в сокет монитора
class Logger {
public:
    static void log(const std::string& message) {
        std::cout << message << std::endl;
        // Логирование в сокеты мониторов
        std::string log_message = message + '\n';
        std::lock_guard<std::mutex> lock(monitor_socket_mutex);
        for (auto it = monitor_socket_fds.begin(); it != monitor_socket_fds.end(); ) {
            int fd = *it;
            if (send(fd, log_message.c_str(), log_message.length(), 0) < 0) {
                std::cerr << "Ошибка отправки лога в монитор (socket " << fd << "): " << strerror(errno) << std::endl;
                close(fd);
                it = monitor_socket_fds.erase(it);
            } else {
                ++it;
            }
        }
    }
};

struct Task {
    int from_id;
    int to_id;
    int result;
    Task(int f, int t, int r) : from_id(f), to_id(t), result(r) {
        Logger::log("Создана новая задача: от ID:" + std::to_string(f) + " к ID:" + std::to_string(t) 
                  + " результат:" + (r == -1 ? "не определен" : std::to_string(r)));
    }
};

enum SendTaskType {
    REQUEST_CHECK,
    REVIEW_RESULT, 
    GET_QUEUE
};

void send_task(int socket_fd, SendTaskType task_type, int id_to, int id_from, int result) {
    std::string message = "";
    if (task_type == REQUEST_CHECK) {
        message = "check " + std::to_string(id_to) + " " + std::to_string(id_from);
    } else if (task_type == REVIEW_RESULT) {
        message = "reviewed " + std::to_string(id_to) + " " + std::to_string(id_from) + " " + std::to_string(result);
    } else if (task_type == GET_QUEUE) {
        message = "queue " + std::to_string(id_to) + " " + std::to_string(id_from);
    }
    message += "\n";
    
    std::string task_type_str;
    std::string emoji;
    switch (task_type) {
        case REQUEST_CHECK: 
            task_type_str = "запрос проверки"; 
            emoji = "🔍";
            break;
        case REVIEW_RESULT: 
            task_type_str = "результат проверки"; 
            emoji = (result == 1) ? "ПРИНЯТО" : "ОТКЛОНЕНО";
            break;
        case GET_QUEUE: 
            task_type_str = "ответ по очереди"; 
            emoji = "📋";
            break;
    }
    
    Logger::log("Сообщение клиенту (сокет " + std::to_string(socket_fd) + "): \"" 
              + message.substr(0, message.size()-1) + "\" [" + task_type_str + "]");
    send(socket_fd, message.c_str(), message.size(), 0);
}


void sigint_handler(int sig) {
    break_flag = 0;
    Logger::log("SIGINT получен. Подготовка к завершению сервера...");
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        std::cerr << "Использование: " << argv[0] << " <адрес сервера> <порт>" << std::endl;
        return 1;
    }

    std::string host_address = argv[1];
    int port = atoi(argv[2]);

    struct sigaction sa;
    sa.sa_handler = &sigint_handler;
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    
    // В очередях лежат id клиентов, для которых надо проверить код
    std::vector<std::queue<Task> > tasks(3, std::queue<Task>());
    
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        std::cerr << "Ошибка создания сокета" << std::endl;
        return 1;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(host_address.c_str());
    server_addr.sin_port = htons(port);


    if (bind(socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Ошибка привязки сокета" << std::endl;
        return 1;
    }

    if (listen(socket_fd, 3) < 0) {
        std::cerr << "Ошибка при прослушивании" << std::endl;
        return 1;
    }

    Logger::log("Сервер запущен и прослушивает " + host_address + ":" + std::to_string(port));


    int connected_clients = 0;
    std::map<int, int> clients; 
    std::atomic<int> next_id = 0;
    
    while (connected_clients < 3) {
        Logger::log("Ожидание подключения клиентов... (" + std::to_string(connected_clients) + "/3)");
        int client_socket;
        struct sockaddr_in client_address;
        socklen_t client_len = sizeof(client_address);

        if ((client_socket = accept(socket_fd, (struct sockaddr *) &client_address, &client_len)) < 0) {
            Logger::log("Ошибка при принятии клиента");
            return 1;
        }

        char buffer[1024];
        int n = recv(client_socket, buffer, sizeof(buffer), 0);
        std::string message(buffer, n);

        if (message == "client\n") {
            int new_client_id = next_id++;
            clients[new_client_id] = client_socket;
            connected_clients++;

            Logger::log("Клиент #" + std::to_string(connected_clients) + " подключен с ID:" + std::to_string(new_client_id)
                  + " (сокет: " + std::to_string(client_socket) + ", IP: " 
                  + inet_ntoa(client_address.sin_addr) + ":" + std::to_string(ntohs(client_address.sin_port)) + ")");
                  
        } else if (message == "monitor\n") {
            {
                std::lock_guard<std::mutex> lock(monitor_socket_mutex);
                monitor_socket_fds.push_back(client_socket);
            }
            Logger::log("Монитор подключен (сокет: " + std::to_string(client_socket) + ")");
        }
    }

    Logger::log("Все клиенты подключены. Отправка стартовых сообщений...");
    
    for (auto& pair : clients) {
        std::string message = "start " + std::to_string(pair.first) + "\n";
        Logger::log("Отправка ID:" + std::to_string(pair.first) + " клиенту (сокет: " + std::to_string(pair.second) + ")");
        send(pair.second, message.c_str(), message.size(), 0);
    }

    Logger::log("Сервер готов к работе");
    
    // Создаем поток для ожидания подключения монитора
    std::thread monitor_thread([&]() {
        Logger::log("Запущен поток для ожидания подключения монитора");
        
        while (break_flag) {
            int monitor_socket;
            struct sockaddr_in monitor_address;
            socklen_t monitor_len = sizeof(monitor_address);
            
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(socket_fd, &readfds);
            
            struct timeval tv;
            tv.tv_sec = 3;
            tv.tv_usec = 0;
            
            int select_result = select(socket_fd + 1, &readfds, NULL, NULL, &tv);
            
            if (select_result > 0 && FD_ISSET(socket_fd, &readfds)) {
                if ((monitor_socket = accept(socket_fd, (struct sockaddr *) &monitor_address, &monitor_len)) < 0) {
                    Logger::log("Ошибка при принятии соединения монитора");
                    std::this_thread::sleep_for(std::chrono::seconds(5));
                    continue;
                }
                
                char buffer[1024];
                int n = recv(monitor_socket, buffer, sizeof(buffer), 0);
                std::string message(buffer, n);
                
                if (message == "monitor\n") {
                    {
                        std::lock_guard<std::mutex> lock(monitor_socket_mutex);
                        monitor_socket_fds.push_back(monitor_socket);
                    }
                    Logger::log("Монитор подключен (сокет: " + std::to_string(monitor_socket) 
                              + ", IP: " + inet_ntoa(monitor_address.sin_addr) 
                              + ":" + std::to_string(ntohs(monitor_address.sin_port)) + ")");
                } else {
                    close(monitor_socket);
                }
            }
            
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
        Logger::log("Поток ожидания монитора завершен");
    });
    
    monitor_thread.detach();

    // Обработка одновременно трех клиентов параллельно
    for (int i = 0; i < 3; i++) {
        std::thread([&, i]() {
            int socket_fd = clients[i];
            char buffer[1024];

            std::string recv_buffer;
            Logger::log("Запущен поток для клиента ID:" + std::to_string(i) + " (сокет: " + std::to_string(socket_fd) + ")");

            while (break_flag) {
                int n = recv(socket_fd, buffer, sizeof(buffer), 0);
                if (n <= 0) {
                    Logger::log("Клиент ID:" + std::to_string(i) + " отключился или произошла ошибка чтения");
                    return;
                }

                
                recv_buffer.append(buffer, n);
                size_t pos = 0;
                std::string message;
                while ((pos = recv_buffer.find('\n')) != std::string::npos) {
                    message = recv_buffer.substr(0, pos);
                    Logger::log("Получено сообщение от клиента ID:" + std::to_string(i) + ": \"" + message + "\"");
                    recv_buffer.erase(0, pos + 1);
                    if (message.empty()) {
                        Logger::log("Получено пустое сообщение от клиента ID:" + std::to_string(i));
                        continue;
                    }
                    if (message.substr(0, 5) == "check") {
                        std::istringstream iss(message);
                        std::string cmd;
                        int to, from;
                        iss >> cmd >> to >> from;
                        Logger::log("Клиент ID:" + std::to_string(from) + " запрашивает проверку у клиента ID:" + std::to_string(to));
                        tasks[to].push(Task{from, to, -1});
    
                        Logger::log("Задача добавлена в очередь клиента ID:" + std::to_string(to));

                        send_task(clients[to], REQUEST_CHECK, to, from, 0);
                    } else if (message.substr(0, 8) == "reviewed") {
                        std::istringstream iss(message);
                        std::string cmd;
                        int to, from, result;
                        iss >> cmd >> to >> from >> result;
                        std::string result_str = (result == 1) ? "ПРИНЯТО" : "ОТКЛОНЕНО";
                        Logger::log("Клиент ID:" + std::to_string(from) + " проверил клиента ID:" + std::to_string(to) 
                                  + " с результатом: " + result_str);
    
                        send_task(clients[to], REVIEW_RESULT, to, from, result);
                    } else if (message.substr(0, 5) == "queue") {
                        std::istringstream iss(message);
                        std::string cmd;
                        int id;
                        iss >> cmd >> id;
                        Logger::log("Клиент ID:" + std::to_string(id) + " запрашивает задачи из очереди");
                        if (tasks[id].empty()) {
                            Logger::log("Очередь для клиента ID:" + std::to_string(id) + " пуста");
                            send_task(clients[id], GET_QUEUE, -1, id, 0);
                        } else {
                            Logger::log("Отправка следующей задачи клиенту ID:" + std::to_string(id) 
                                      + " (от клиента ID:" + std::to_string(tasks[id].front().from_id) + ")");
                            send_task(clients[id], GET_QUEUE, tasks[id].front().from_id, id, 0);
                            tasks[id].pop();
                        }
                    } else {
                        Logger::log("Неизвестное сообщение от клиента ID:" + std::to_string(i) + ": \"" + message + "\"");
                    }
                }
            }

            Logger::log("Закрытие сокета для клиента ID:" + std::to_string(i));
            close(socket_fd);
        }).detach();
    }

    Logger::log("Сервер работает. Нажмите Ctrl+C для завершения...");

    while (break_flag) {
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }

    Logger::log("Сервер завершает работу...");

    close(socket_fd);
    for (auto& pair : clients) {
        Logger::log("Закрытие сокета клиента ID:" + std::to_string(pair.first) + " (сокет: " + std::to_string(pair.second) + ")");
        close(pair.second);
    }

    Logger::log("Сервер успешно завершил работу");
    return 0;
}