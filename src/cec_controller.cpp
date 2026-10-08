#include "cec_controller.h"

#include <chrono>
#include <fcntl.h>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

CecController::CecController(bool volume_enabled) : volume_enabled_(volume_enabled) {}

CecController::~CecController() { stop(); }

bool CecController::start() {
    int fds[2];
    if (pipe(fds) != 0) return false;

    pid_ = fork();
    if (pid_ < 0) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    if (pid_ == 0) {  // child
        dup2(fds[0], STDIN_FILENO);
        close(fds[0]);
        close(fds[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        char* const args[] = {const_cast<char*>("cec-client"), const_cast<char*>("-d"),
                              const_cast<char*>("1"), const_cast<char*>("-t"),
                              const_cast<char*>("p"), const_cast<char*>("-o"),
                              const_cast<char*>("Sendspin"), nullptr};
        execvp("cec-client", args);
        _exit(127);  // exec failed
    }

    close(fds[0]);
    fd_ = fds[1];
    worker_ = std::thread([this] { run(); });
    return true;
}

void CecController::stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stop_) return;
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (fd_ >= 0) {
        (void)!write(fd_, "q\n", 2);
        close(fd_);  // EOF on stdin also tells cec-client to exit
        fd_ = -1;
    }
    if (pid_ > 0) {
        kill(pid_, SIGTERM);
        waitpid(pid_, nullptr, 0);
        pid_ = -1;
    }
}

void CecController::wake() {
    if (awake_.exchange(true)) return;
    push("on 0\nas\n", 0);
}

void CecController::standby() {
    if (!awake_.exchange(false)) return;
    push("standby 0\n", 0);
}

void CecController::set_volume(uint8_t vol) {
    int prev = last_volume_.exchange(vol);
    if (prev < 0) return;  // first call just sets the baseline

    int delta = static_cast<int>(vol) - prev;
    int n = delta < 0 ? -delta : delta;
    if (n > 20) n = 20;  // don't flood the bus on big jumps

    const char* press = delta > 0 ? "tx 40:44:41\ntx 40:45\n"   // volume up
                                  : "tx 40:44:42\ntx 40:45\n";  // volume down
    for (int i = 0; i < n; ++i) push(press, 150);
}

void CecController::set_muted(bool muted) {
    if (muted_.exchange(muted) == muted) return;
    push("tx 40:44:43\ntx 40:45\n", 150);  // mute is a toggle
}

void CecController::push(std::string cmd, int delay_ms) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        q_.push_back({std::move(cmd), delay_ms});
    }
    cv_.notify_one();
}

void CecController::run() {
    std::unique_lock<std::mutex> lk(mu_);
    while (true) {
        cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
        if (stop_) break;

        Item item = std::move(q_.front());
        q_.pop_front();
        lk.unlock();

        (void)!write(fd_, item.cmd.data(), item.cmd.size());
        if (item.delay_ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(item.delay_ms));

        lk.lock();
    }
}