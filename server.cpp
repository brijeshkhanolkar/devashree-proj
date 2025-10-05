// server.cpp
// Cross-platform minimal C++ HTTP server (single-threaded) with login/register/quiz/myresults
// Compile on Linux/Mac: g++ server.cpp -o server
// Compile on Windows (MinGW): g++ server.cpp -o server.exe -lws2_32

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <map>
#include <vector>
#include <algorithm>
#include <cctype>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef SOCKET sock_t;
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  typedef int sock_t;
  #define INVALID_SOCKET -1
#endif

using namespace std;

string urlDecode(const string &s) {
    string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '+') out.push_back(' ');
        else if (c == '%' && i + 2 < s.size()) {
            string hex = s.substr(i+1,2);
            char decoded = (char) strtol(hex.c_str(), nullptr, 16);
            out.push_back(decoded);
            i += 2;
        } else out.push_back(c);
    }
    return out;
}

map<string,string> parseQuery(const string &q) {
    map<string,string> m;
    stringstream ss(q);
    string pair;
    while (getline(ss, pair, '&')) {
        size_t pos = pair.find('=');
        if (pos != string::npos) {
            string k = urlDecode(pair.substr(0,pos));
            string v = urlDecode(pair.substr(pos+1));
            m[k] = v;
        }
    }
    return m;
}

string readFile(const string &filename) {
    ifstream f(filename, ios::in | ios::binary);
    if (!f) return "";
    stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool userExists(const string &username, const string &password) {
    ifstream f("users.txt");
    if (!f) return false;
    string line;
    while (getline(f, line)) {
        if (line.empty()) continue;
        // stored as: username<space>password
        string u, p;
        stringstream ss(line);
        ss >> u >> p;
        if (u == username && p == password) return true;
    }
    return false;
}

bool usernameTaken(const string &username) {
    ifstream f("users.txt");
    if (!f) return false;
    string line;
    while (getline(f, line)) {
        string u, p;
        stringstream ss(line);
        ss >> u >> p;
        if (u == username) return true;
    }
    return false;
}

bool registerUser(const string &username, const string &password) {
    if (username.empty() || password.empty()) return false;
    if (username.find('|') != string::npos) return false; // avoid delim char
    if (usernameTaken(username)) return false;
    ofstream f("users.txt", ios::app);
    if (!f) return false;
    f << username << " " << password << "\n";
    return true;
}

map<string,string> parseCookies(const string &headers) {
    map<string,string> cookies;
    size_t pos = headers.find("Cookie:");
    if (pos == string::npos) pos = headers.find("cookie:");
    if (pos == string::npos) return cookies;
    size_t eol = headers.find("\r\n", pos);
    string cookieLine = headers.substr(pos + 7, eol - (pos + 7)); // after "Cookie:"
    // cookieLine like " user=Bob; other=..."
    stringstream ss(cookieLine);
    string token;
    while (getline(ss, token, ';')) {
        size_t eq = token.find('=');
        if (eq != string::npos) {
            string k = token.substr(0, eq);
            string v = token.substr(eq + 1);
            // trim spaces
            while (!k.empty() && isspace((unsigned char)k.front())) k.erase(0,1);
            while (!k.empty() && isspace((unsigned char)k.back())) k.pop_back();
            while (!v.empty() && isspace((unsigned char)v.front())) v.erase(0,1);
            while (!v.empty() && isspace((unsigned char)v.back())) v.pop_back();
            cookies[k] = v;
        }
    }
    return cookies;
}

void sendResponse(sock_t client, const string &status, const string &contentType,
                  const string &body, const vector<string>& extraHeaders = {}) {
    stringstream resp;
    resp << status << "\r\n";
    resp << "Content-Type: " << contentType << "\r\n";
    resp << "Content-Length: " << body.size() << "\r\n";
    for (auto &h : extraHeaders) resp << h << "\r\n";
    resp << "\r\n";
    resp << body;
    string out = resp.str();
#ifdef _WIN32
    send(client, out.c_str(), (int)out.size(), 0);
#else
    send(client, out.c_str(), out.size(), 0);
#endif
}

int main() {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0) {
        cerr << "WSAStartup failed\n"; return 1;
    }
#endif

    sock_t server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == INVALID_SOCKET) {
        cerr << "socket() failed\n"; return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    int port = 8080;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        cerr << "bind() failed (port " << port << " may be in use)\n";
#ifdef _WIN32
        closesocket(server_fd);
        WSACleanup();
#else
        close(server_fd);
#endif
        return 1;
    }

    listen(server_fd, 10);
    cout << "Server running at http://localhost:" << port << "  (Ctrl-C to stop)\n";

    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        sock_t client = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client == INVALID_SOCKET) {
            cerr << "accept() failed\n";
            continue;
        }

        // Read request (simple loop until blank line)
        string request;
        const int BUF_SIZE = 8192;
        char buffer[BUF_SIZE];
        int n;
#ifdef _WIN32
        n = recv(client, buffer, BUF_SIZE-1, 0);
#else
        n = recv(client, buffer, BUF_SIZE-1, 0);
#endif
        if (n <= 0) {
#ifdef _WIN32
            closesocket(client);
#else
            close(client);
#endif
            continue;
        }
        buffer[n] = '\0';
        request = string(buffer, n);

        // parse request line
        size_t pos_crlf = request.find("\r\n");
        string requestLine = request.substr(0, pos_crlf);
        string method, fullPath, protocol;
        {
            stringstream ss(requestLine);
            ss >> method >> fullPath >> protocol;
        }
        // split path and query
        string path = fullPath;
        string query;
        size_t qpos = fullPath.find('?');
        if (qpos != string::npos) {
            path = fullPath.substr(0, qpos);
            query = fullPath.substr(qpos + 1);
        }

        // parse cookies to identify logged user
        auto cookies = parseCookies(request);
        string loggedUser = "";
        if (cookies.count("user")) loggedUser = cookies["user"];

        // Route handling
        if (method == "GET" && (path == "/" || path == "/index.html")) {
            string html = readFile("index.html");
            if (html.empty()) {
                html = "<html><body><h1>index.html not found</h1></body></html>";
            }
            sendResponse(client, "HTTP/1.1 200 OK", "text/html", html);
        }
        else if (method == "GET" && path == "/style.css") {
            string css = readFile("style.css");
            if (css.empty()) css = "/* style.css not found */";
            sendResponse(client, "HTTP/1.1 200 OK", "text/css", css);
        }
        else if (method == "GET" && path == "/quiz.html") {
            if (loggedUser.empty()) {
                // redirect to login
                vector<string> hdrs = {"Location: /"};
                sendResponse(client, "HTTP/1.1 302 Found", "text/html", "<html><body>Redirecting...</body></html>", hdrs);
            } else {
                string html = readFile("quiz.html");
                if (html.empty()) html = "<html><body><h1>quiz.html not found</h1></body></html>";
                sendResponse(client, "HTTP/1.1 200 OK", "text/html", html);
            }
        }
        else if (method == "GET" && path == "/register") {
            // quick register page (if you don't have a file)
            string body = R"(
<html><body style='font-family:Arial;text-align:center;'>
<h1>Register</h1>
<form method="GET" action="/newuser">
<input name="username" placeholder="username" required><br>
<input name="password" placeholder="password" required><br>
<button type="submit">Register</button>
</form>
<a href="/">Back to Login</a>
</body></html>
)";
            sendResponse(client, "HTTP/1.1 200 OK", "text/html", body);
        }
        else if (method == "GET" && path == "/newuser") {
            auto params = parseQuery(query);
            string u = params["username"];
            string p = params["password"];
            string body;
            if (registerUser(u,p)) {
                body = "<html><body><h1>Registration successful</h1><a href='/'>Login</a></body></html>";
            } else {
                body = "<html><body><h1>Registration failed (username may exist or invalid)</h1><a href='/register'>Try again</a></body></html>";
            }
            sendResponse(client, "HTTP/1.1 200 OK", "text/html", body);
        }
        else if (method == "GET" && path == "/login") {
            auto params = parseQuery(query);
            string u = params["username"];
            string p = params["password"];
            if (userExists(u,p)) {
                // successful login -> set cookie + redirect to quiz
                vector<string> hdrs = { string("Set-Cookie: user=" + u + "; Path=/; HttpOnly"),
                                        string("Location: /quiz.html") };
                sendResponse(client, "HTTP/1.1 302 Found", "text/html", "<html><body>Login OK, redirecting...</body></html>", hdrs);
            } else {
                string body = "<html><body><h1>Invalid credentials</h1><a href='/'>Back</a></body></html>";
                sendResponse(client, "HTTP/1.1 200 OK", "text/html", body);
            }
        }
        else if (method == "GET" && path == "/quiz") {
            if (loggedUser.empty()) {
                vector<string> hdrs = {"Location: /"};
                sendResponse(client, "HTTP/1.1 302 Found", "text/html", "<html><body>Redirect...</body></html>", hdrs);
            } else {
                auto params = parseQuery(query);
                int score = 0;
                if (params["q1"] == "4") score++;
                if (params["q2"] == "Paris") score++;
                if (params["q3"] == "System Programming") score++;

                // save to results.txt: username|q1,q2,q3,score
                ofstream out("results.txt", ios::app);
                if (out) {
                    out << loggedUser << "|" 
                        << params["q1"] << "," 
                        << params["q2"] << "," 
                        << params["q3"] << "," 
                        << score << "\n";
                }

                stringstream ss;
                ss << "<html><body style='font-family:Arial;text-align:center;'>"
                   << "<h1>Hello " << loggedUser << "!</h1>"
                   << "<h2>Your Score: " << score << "/3</h2>"
                   << "<a href='/quiz.html'>Try Again</a><br><br>"
                   << "<a href='/myresults'>View My Results</a>"
                   << "</body></html>";
                sendResponse(client, "HTTP/1.1 200 OK", "text/html", ss.str());
            }
        }
        else if (method == "GET" && path == "/myresults") {
            if (loggedUser.empty()) {
                vector<string> hdrs = {"Location: /"};
                sendResponse(client, "HTTP/1.1 302 Found", "text/html", "<html><body>Redirect...</body></html>", hdrs);
            } else {
                ifstream in("results.txt");
                string line, rows;
                while (getline(in, line)) {
                    if (line.rfind(loggedUser + "|", 0) == 0) {
                        string rest = line.substr(loggedUser.size() + 1);
                        // rest = q1,q2,q3,score
                        stringstream ss(rest);
                        string q1,q2,q3,score;
                        getline(ss, q1, ','); getline(ss, q2, ','); getline(ss, q3, ','); getline(ss, score, ',');
                        rows += "Q1: " + q1 + ", Q2: " + q2 + ", Q3: " + q3 + " | Score: " + score + "/3<br>";
                    }
                }
                if (rows.empty()) rows = "No attempts yet!";
                string body = "<html><body style='font-family:Arial;text-align:center;'><h1>" + loggedUser + "'s Quiz History</h1>" + rows + "<br><a href='/quiz.html'>Back to Quiz</a></body></html>";
                sendResponse(client, "HTTP/1.1 200 OK", "text/html", body);
            }
        }
        else {
            // 404
            string body = "<html><body><h1>404 Not Found</h1><a href='/'>Home</a></body></html>";
            sendResponse(client, "HTTP/1.1 404 Not Found", "text/html", body);
        }

        // close client
#ifdef _WIN32
        closesocket(client);
#else
        close(client);
#endif
    }

#ifdef _WIN32
    closesocket(server_fd);
    WSACleanup();
#else
    close(server_fd);
#endif

    return 0;
}
