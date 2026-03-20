#include <iostream>
#include <string>
#include <vector>
#include <stdexcept>
#include "utils.hpp"

class App {
public:
    App(const std::string &name, int log_level)
        : name_(name), log_level_(log_level), running_(false) {}

    void configure(const std::vector<std::string> &args) {
        for (const auto &arg : args) {
            int val = utils::parse_value(arg);
            if (val >= 0) {
                log_level_ = val;
            }
            std::string trimmed = utils::trim(arg);
            if (!trimmed.empty()) {
                config_entries_.push_back(trimmed);
            }
        }
        std::cout << utils::format_string("configured", name_) << std::endl;
    }

    int run() {
        if (config_entries_.empty()) {
            std::cerr << "no configuration loaded" << std::endl;
            return 1;
        }
        running_ = true;
        std::cout << "running " << name_ << " with " << config_entries_.size()
                  << " config entries (log_level=" << log_level_ << ")" << std::endl;
        for (const auto &entry : config_entries_) {
            std::cout << "  processing: " << entry << std::endl;
        }
        running_ = false;
        return 0;
    }

private:
    std::string name_;
    int log_level_;
    bool running_;
    std::vector<std::string> config_entries_;
};

int main(int argc, char *argv[]) {
    App app("myapp", 1);
    std::vector<std::string> args(argv + 1, argv + argc);
    app.configure(args);
    return app.run();
}
