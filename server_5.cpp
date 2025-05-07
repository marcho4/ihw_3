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

int break_flag = 1;

struct Task {
    int from_id;
    int to_id;
    int result;
    Task(int f, int t, int r) : from_id(f), to_id(t), result(r) {
        std::cout << "Создана новая задача: от ID:" << f << " к ID:" << t 
                  << " результат:" << (r == -1 ? "не определен" : std::to_string(r)) << std::endl;
    }
};

enum SendTaskType {
    REQUEST_CHECK,
    REVIEW_RESULT, 
    GET_QUEUE
};

enum State {
    WRITING,
    WAITING,
    FIXING,
    REVIEWING,
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
    
    std::cout << "Сообщение клиенту (сокет " << socket_fd << "): \"" 
              << message.substr(0, message.size()-1) << "\" [" << task_type_str << "]" << std::endl;
    send(socket_fd, message.c_str(), message.size(), 0);
}


void sigint_handler(int sig) {
    break_flag = 0;
    std::cout << "SIGINT получен. Подготовка к завершению сервера..." << std::endl;
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

    std::cout << "Сервер запущен и прослушивает " << host_address << ":" << port << std::endl;

    int connected_clients = 0;
    std::map<int, int> clients; // id - socket
    std::atomic<int> next_id = 0;
    
    while (connected_clients < 3) {
        std::cout << "Ожидание подключения клиентов... (" << connected_clients << "/3)" << std::endl;
        int client_socket;
        struct sockaddr_in client_address;
        socklen_t client_len = sizeof(client_address);

        if ((client_socket = accept(socket_fd, (struct sockaddr *) &client_address, &client_len)) < 0) {
            std::cout << "Ошибка при принятии клиента" << std::endl;
            return 1;
        }

        int new_client_id = next_id++;
        clients[new_client_id] = client_socket;

        std::cout << "Клиент #" << connected_clients << " подключен с ID:" << new_client_id
                  << " (сокет: " << client_socket << ", IP: " 
                  << inet_ntoa(client_address.sin_addr) << ":" << ntohs(client_address.sin_port) << ")" << std::endl;
        connected_clients++;
    }

    std::cout << "Все клиенты подключены. Отправка стартовых сообщений..." << std::endl;
    
    for (auto& pair : clients) {
        std::string message = "start " + std::to_string(pair.first) + "\n";
        std::cout << "Отправка ID:" << pair.first << " клиенту (сокет: " << pair.second << ")" << std::endl;
        send(pair.second, message.c_str(), message.size(), 0);
    }

    std::cout << "Сервер готов к работе" << std::endl;

    // Обработка одновременно трех клиентов параллельно
    for (int i = 0; i < 3; i++) {
        std::thread([&, i]() {
            int socket_fd = clients[i];
            char buffer[1024];

            std::string recv_buffer;
            std::cout << "Запущен поток для клиента ID:" << i << " (сокет: " << socket_fd << ")" << std::endl;

            while (break_flag) {
                int n = recv(socket_fd, buffer, sizeof(buffer), 0);
                if (n <= 0) {
                    std::cout << "Клиент ID:" << i << " отключился или произошла ошибка чтения" << std::endl;
                    return;
                }

                
                recv_buffer.append(buffer, n);
                size_t pos = 0;
                std::string message;
                while ((pos = recv_buffer.find('\n')) != std::string::npos) {
                    message = recv_buffer.substr(0, pos);
                    std::cout << "Получено сообщение от клиента ID:" << i << ": \"" << message << "\"" << std::endl;
                    recv_buffer.erase(0, pos + 1);
                    if (message.empty()) {
                        std::cout << "Получено пустое сообщение от клиента ID:" << i << std::endl;
                        continue;
                    }
                    if (message.substr(0, 5) == "check") {
                        std::istringstream iss(message);
                        std::string cmd;
                        int to, from;
                        iss >> cmd >> to >> from;
                        std::cout << "Клиент ID:" << from << " запрашивает проверку у клиента ID:" << to << std::endl;
                        tasks[to].push(Task{from, to, -1});
    
                        std::cout << "Задача добавлена в очередь клиента ID:" << to << std::endl;

                        send_task(clients[to], REQUEST_CHECK, to, from, 0);
                    } else if (message.substr(0, 8) == "reviewed") {
                        std::istringstream iss(message);
                        std::string cmd;
                        int to, from, result;
                        iss >> cmd >> to >> from >> result;
                        std::string result_str = (result == 1) ? "ПРИНЯТО" : "ОТКЛОНЕНО";
                        std::cout << "Клиент ID:" << from << " проверил клиента ID:" << to 
                                  << " с результатом: " << result_str << std::endl;
    
                        send_task(clients[to], REVIEW_RESULT, to, from, result);
                    } else if (message.substr(0, 5) == "queue") {
                        std::istringstream iss(message);
                        std::string cmd;
                        int id;
                        iss >> cmd >> id;
                        std::cout << "Клиент ID:" << id << " запрашивает задачи из очереди" << std::endl;
                        if (tasks[id].empty()) {
                            std::cout << "Очередь для клиента ID:" << id << " пуста" << std::endl;
                            send_task(clients[id], GET_QUEUE, -1, id, 0);
                        } else {
                            std::cout << "Отправка следующей задачи клиенту ID:" << id 
                                      << " (от клиента ID:" << tasks[id].front().from_id << ")" << std::endl;
                            send_task(clients[id], GET_QUEUE, tasks[id].front().from_id, id, 0);
                            tasks[id].pop();
                        }
                    } else {
                        std::cout << "Неизвестное сообщение от клиента ID:" << i << ": \"" << message << "\"" << std::endl;
                    }
                }
            }

            std::cout << "Закрытие сокета для клиента ID:" << i << std::endl;
            close(socket_fd);
        }).detach();
    }

    std::cout << "Сервер работает. Нажмите Ctrl+C для завершения..." << std::endl;

    while (break_flag) {
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }

    std::cout << "Сервер завершает работу..." << std::endl;

    close(socket_fd);
    for (auto& pair : clients) {
        std::cout << "Закрытие сокета клиента ID:" << pair.first << " (сокет: " << pair.second << ")" << std::endl;
        close(pair.second);
    }

    std::cout << "Сервер успешно завершил работу" << std::endl;
    return 0;
}