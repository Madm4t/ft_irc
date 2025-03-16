
#include "Server.hpp"
#include "../Channel/Channel.hpp"

Server::Server()
{
    _client.reserve(100);
}

Server::~Server()
{
    std::cout << "Shutting down server..." << std::endl;

    for (auto &client : this->_client)
    {
        close(client.get_sockfd());
    }
    this->_client.clear();
    this->_poll_fd.clear();
    this->_channel.clear();
    close(this->_sockfd);
}

Server::msg_tokens Server::error_message(std::string error_code, std::string message)
{
    msg_tokens error_message;
    error_message.command = error_code;
    error_message.params.push_back(message);
    return (error_message);
}

void Server::send_error_message(int client_index, std::string error_code, std::string message)
{
    msg_tokens error_msg = error_message(error_code, message);
    std::string formatted_error = ":" + error_msg.command + " " + error_msg.params[0] + "\n";
    putstr_fd(formatted_error, this->_client[client_index].get_sockfd());
}

void Server::create_socket()
{
    this->_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (this->_sockfd < 0)
        throw std::invalid_argument ("ERROR opening socket");
}

void Server::fill_socket_struct()
{
    int enabled = 1;

    bzero((char *) &this->_server_address, sizeof(this->_server_address));
    this->_server_address.sin_family = AF_INET;
    this->_server_address.sin_addr.s_addr = INADDR_ANY;
    this->_server_address.sin_port = htons(this->_port);
    if (setsockopt(this->_sockfd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0)
        throw std::invalid_argument ("ERROR on setsockopt");
    if (fcntl(this->_sockfd, F_SETFL, O_NONBLOCK) < 0)
        throw std::invalid_argument ("ERROR on fcntl");
}

void Server::bind_server_address()
{
    if (bind(this->_sockfd, (struct sockaddr *) &this->_server_address, sizeof(this->_server_address)) < 0)
        throw std::invalid_argument ("ERROR on binding");
}

void Server::init_poll_struct(int fd)
{
    struct pollfd new_poll;

    memset(&new_poll, 0, sizeof(new_poll));
    new_poll.fd = fd;
    new_poll.events = POLLIN;
    new_poll.revents = 0;
    this->_poll_fd.emplace_back(new_poll);
}

void Server::init(char **av)
{
    this->create_socket();
    this->_port = std::atoi(av[1]);
	this->_password = av[2];
    this->fill_socket_struct();
    this->bind_server_address();
    listen(this->_sockfd, 5);
    this->init_poll_struct(this->_sockfd);
    this->running = true;
}

void Server::accept_client()
{
    Client               new_client;
    struct sockaddr_in   temp_client_address;
    socklen_t            temp_client_len;

    new_client.set_nick_name((char *)"none");
    new_client.set_len(sizeof(new_client.get_address()));
    temp_client_len = new_client.get_len();
    new_client.set_sockfd(accept(this->_sockfd, (struct sockaddr *)&temp_client_address, &temp_client_len));
    if (new_client.get_sockfd() < 0)
        throw std::invalid_argument ("ERROR on accept");
    this->_client.emplace_back(new_client);
    this->init_poll_struct(new_client.get_sockfd());
}

void Server::disconnect_client(int client_index)
{
    std::string nick = this->_client[client_index].get_nick_name();
    std::cout << "Client " << nick << " disconnected." << std::endl;

    int client_fd = this->_client[client_index].get_sockfd();
    close(client_fd);
    for (size_t i = 1; i < _poll_fd.size(); ++i)
    {
        if (_poll_fd[i].fd == client_fd)
        {
            _poll_fd.erase(_poll_fd.begin() + i);
            break;
        }
    }
    _client.erase(_client.begin() + client_index);
}

//not a fatal error should not quit whole server
// void Server::receive_data(int client_index)
// {
//     char    buffer[1028];
//     int     n = read(this->_client[client_index].get_sockfd(), buffer, sizeof(buffer) - 1);

//     if (n < 0)
//         throw std::invalid_argument ("ERROR reading from socket");
//     this->_client[client_index].set_last_message(buffer);
// } // debug

void Server::receive_data(int client_index)
{
	if (client_index >= static_cast<int>(_client.size()))
		return;

    char buffer[1028];
    int client_fd = _client[client_index].get_sockfd();
    int n = read(client_fd, buffer, sizeof(buffer) - 1);

    if (n < 0)
    {
        if (errno == EWOULDBLOCK || errno == EAGAIN)
            return;

        std::cerr << "ERROR reading from socket (Client: "
                  << _client[client_index].get_nick_name()
                  << "): " << strerror(errno) << std::endl;

        send_error_message(client_index, "ERROR", strerror(errno));
        disconnect_client(client_index);
        return;
    }
    if (n == 0)
    {
        disconnect_client(client_index);
        return;
    }

    buffer[n] = '\0';
    _client[client_index].append_last_message(buffer);
}

Server::msg_tokens Server::parse_message_line(std::string line)
{
    std::stringstream           line_stream(line);
    std::string                 word;
    std::string                 trailing_substr;
    struct msg_tokens           tokenized_message;

	if (line.empty())
	    return (error_message("421", "Unknown"));
    if (line[0] == ':')
        line_stream >> tokenized_message.prefix;
    if (!(line_stream >> tokenized_message.command))
		return (error_message("421", "Unknown"));
    while (line_stream >> word)
    {
        if (word[0] == ':')
        {
            std::getline(line_stream, trailing_substr);
            tokenized_message.trailing = word.substr(1) + trailing_substr;
            break ;
        }
        tokenized_message.params.emplace_back(word);
    }
    return (tokenized_message);
}

void Server::handle_data(int client_index)
{
	std::stringstream       message_stream(this->_client[client_index].get_last_message());
	std::string             line;
	struct msg_tokens       tokenized_message;

	while (std::getline(message_stream, line))
	{
		if (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
			line.pop_back();
		if (!line.empty())
		{
			tokenized_message = this->parse_message_line(line);
            std::cout << this->_client[client_index].get_nick_name() << ": " << line << std::endl;
			this->execute_command(tokenized_message, client_index);
		}
	}
}

        // std::cout << "prefix: " << tokenized_message.prefix << std::endl;
        // std::cout << "command: " << tokenized_message.command << std::endl;
        // std::cout << "params: ";
        // for (auto &it : tokenized_message.params)
        // {
        //     std::cout << it << "-";
        // }
        // std::cout << std::endl;
        // std::cout << "trailing: " << tokenized_message.trailing << std::endl;

void Server::loop()
{
	while (this->running)
	{
		if (poll(&_poll_fd[0], _poll_fd.size(), -1) == -1)
			throw std::invalid_argument("ERROR during poll");
		for (int i = _poll_fd.size() - 1; i >= 0; --i)
		{
			if (_poll_fd[i].revents & POLLIN)
			{
				if (i == 0)
					accept_client();
				else
				{
					int client_index = i - 1;
					receive_data(client_index);
					if (client_index < static_cast<int>(_client.size()) && this->_client[client_index].get_last_message().back() == '\n')
						handle_data(client_index);
				}
			}
		}
		for (auto it = _client.begin(); it != _client.end();)
		{
			if (it->get_sockfd() == -1)
				it = _client.erase(it);
			else
				++it;
		}
	}
}

void Server::end()
{
    for (auto &it : this->_client)
    {
        close (it.get_sockfd());
    }
    this->_client.clear();

    for (auto &it : this->_poll_fd)
    {
        close (it.fd);
    }
    this->_poll_fd.clear();
}

bool Server::authenticateClient(const msg_tokens &tokenized_message, int client_index)
{
	std::string password;
	if (!tokenized_message.params.empty())
		password = tokenized_message.params[0];
	else
		password = tokenized_message.trailing;
	//std::cout << "Print password:" << password << std::endl;
	//std::cout << "Print server password:" << this->_password << std::endl;
	if (password != this->_password)
	{
		this->_client[client_index].increase_failed_auth_attempts();
		if (this->_client[client_index].get_failed_auth_attempts() >= 3)
		{
			std::string error_msg = ":server 464 " + this->_client[client_index].get_nick_name() +
									" :Too many failed authentication attempts. Disconnecting...\n";
			putstr_fd(error_msg, this->_client[client_index].get_sockfd());
			std::string quit_msg = ":" + this->_client[client_index].get_nick_name() +
								" QUIT :Excessive failed authentication\n";
			putstr_fd(quit_msg, this->_client[client_index].get_sockfd());
			disconnect_client(client_index);
			return (false);
		}
		std::string passwd_mismatch_msg = ":server 464 " + this->_client[client_index].get_nick_name() +
										" :Incorrect password. Try again.\n";
		putstr_fd(passwd_mismatch_msg, this->_client[client_index].get_sockfd());
		return (false);
	}
	this->_client[client_index].reset_failed_auth_attempts();
	this->_client[client_index].set_authenticated(true);
	return (true);
}
