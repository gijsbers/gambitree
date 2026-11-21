#include "gambitree/session_state.h"
#include "gambitree/piece_set.h"
#include "gambitree/theme.h"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <new>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cctype>
#include <fcntl.h>
#include <limits>

namespace gambitree {

namespace {

const int kDefaultWidth = 900;
const int kDefaultHeight = 650;
const std::size_t kMaximumStateBytes = 65536U;
const uint64_t kSaveDelayMilliseconds = 1000;
const uint64_t kSaveRetryMilliseconds = 5000;

bool parse_integer(const std::string& text, long& value)
{
    char* end = 0;
    std::string::size_type index = 0;
    long parsed;
    if (text.empty())
        return false;
    if (text[0] == '-')
        index = 1;
    if (index == text.size())
        return false;
    for (; index < text.size(); ++index) {
        if (!std::isdigit(static_cast<unsigned char>(text[index])))
            return false;
    }
    errno = 0;
    parsed = std::strtol(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || *end != '\0')
        return false;
    value = parsed;
    return true;
}

bool parse_dimension(const std::string& text, int minimum, int& result)
{
    long value;
    if (!parse_integer(text, value) || value < minimum || value > 8192)
        return false;
    result = static_cast<int>(value);
    return true;
}

bool parse_position(const std::string& text, int& result)
{
    long value;
    if (!parse_integer(text, value) || value < -1000000L ||
        value > 1000000L)
        return false;
    result = static_cast<int>(value);
    return true;
}

bool ensure_directory_tree(const std::string& path)
{
    std::size_t position = path[0] == '/' ? 1U : 0U;
    while (position <= path.size()) {
        std::size_t slash = path.find('/', position);
        std::string part = path.substr(0, slash == std::string::npos
                                      ? path.size() : slash);
        if (!part.empty()) {
            struct stat info;
            if (stat(part.c_str(), &info) == 0) {
                if (!S_ISDIR(info.st_mode)) {
                    errno = ENOTDIR;
                    return false;
                }
            } else if (errno != ENOENT || mkdir(part.c_str(), 0700) != 0) {
                if (errno != EEXIST || stat(part.c_str(), &info) != 0 ||
                    !S_ISDIR(info.st_mode))
                    return false;
            }
        }
        if (slash == std::string::npos)
            break;
        position = slash + 1;
    }
    return true;
}

std::string parent_directory(const std::string& path)
{
    std::string::size_type slash = path.find_last_of('/');
    if (slash == std::string::npos)
        return ".";
    if (slash == 0)
        return "/";
    return path.substr(0, slash);
}

bool same_position(const SessionState& a, const SessionState& b)
{
    return a.has_position == b.has_position && (!a.has_position ||
        (a.x == b.x && a.y == b.y && a.position_is_outer == b.position_is_outer));
}

bool same_state(const SessionState& a, const SessionState& b)
{
    return a.width == b.width && a.height == b.height && same_position(a, b) &&
        a.analysis_enabled == b.analysis_enabled && a.board_share == b.board_share &&
        a.figurines == b.figurines && a.notation_points == b.notation_points &&
        a.engine_points == b.engine_points && a.piece_set_id == b.piece_set_id &&
        a.board_theme_id == b.board_theme_id;
}

void copy_position(const SessionState& from, SessionState& to)
{
    to.x = from.x; to.y = from.y;
    to.has_position = from.has_position;
    to.position_is_outer = from.position_is_outer;
}

void merge_local_changes(const SessionState& base, const SessionState& local,
                          SessionState& latest)
{
    if (local.width != base.width) latest.width = local.width;
    if (local.height != base.height) latest.height = local.height;
    if (!same_position(base, local)) copy_position(local, latest);
    if (local.analysis_enabled != base.analysis_enabled) latest.analysis_enabled = local.analysis_enabled;
    if (local.board_share != base.board_share) latest.board_share = local.board_share;
    if (local.figurines != base.figurines) latest.figurines = local.figurines;
    if (local.notation_points != base.notation_points) latest.notation_points = local.notation_points;
    if (local.engine_points != base.engine_points) latest.engine_points = local.engine_points;
    if (local.piece_set_id != base.piece_set_id) latest.piece_set_id = local.piece_set_id;
    if (local.board_theme_id != base.board_theme_id) latest.board_theme_id = local.board_theme_id;
    // Never relabel version-1 client coordinates as version-2 outer coordinates.
    if (latest.has_position && !latest.position_is_outer) {
        copy_position(local, latest);
        latest.has_position = local.has_position && local.position_is_outer;
    }
}

uint64_t after_milliseconds(uint64_t now, uint64_t delay)
{
    const uint64_t maximum = std::numeric_limits<uint64_t>::max();
    return now > maximum - delay ? maximum : now + delay;
}

class SessionFileLock {
public:
    SessionFileLock() : fd_(-1) {}
    ~SessionFileLock() {
        const int saved_errno = errno;
        if (fd_ >= 0) close(fd_);
        errno = saved_errno;
    }
    SessionSaveResult acquire(const std::string& path) {
        fd_ = open((path + ".lock").c_str(), O_RDWR | O_CREAT, 0600);
        if (fd_ < 0 || fcntl(fd_, F_SETFD, FD_CLOEXEC) < 0)
            return SESSION_SAVE_FAILED;
        struct flock lock;
        std::memset(&lock, 0, sizeof(lock));
        lock.l_type = F_WRLCK;
        lock.l_whence = SEEK_SET;
        if (fcntl(fd_, F_SETLK, &lock) == 0) return SESSION_SAVE_SAVED;
        return errno == EACCES || errno == EAGAIN
            ? SESSION_SAVE_BUSY : SESSION_SAVE_FAILED;
    }
private:
    int fd_;
    SessionFileLock(const SessionFileLock&);
    SessionFileLock& operator=(const SessionFileLock&);
};

}

SessionStateSaver::SessionStateSaver(const std::string& path,
                                     const SessionState& initial)
    : path_(path), saved_(initial), observed_(initial), deadline_(0),
      retry_at_(0), position_pending_(false) {}

void SessionStateSaver::observe(const SessionState& state, uint64_t now,
                                 bool position_hint_changed)
{
    if (same_state(observed_, state) && !position_hint_changed) return;
    observed_ = state;
    position_pending_ = position_pending_ || position_hint_changed;
    deadline_ = after_milliseconds(now, kSaveDelayMilliseconds);
}

void SessionStateSaver::adopt_initial_geometry(const SessionState& state)
{
    saved_.width = observed_.width = state.width;
    saved_.height = observed_.height = state.height;
    copy_position(state, saved_);
    copy_position(state, observed_);
    position_pending_ = false;
}

bool SessionStateSaver::pending() const
{
    return !path_.empty() && (position_pending_ || !same_state(saved_, observed_));
}

void SessionStateSaver::retain_saved_geometry(SessionState& state) const
{
    state.width = saved_.width;
    state.height = saved_.height;
    copy_position(saved_, state);
}

uint64_t SessionStateSaver::remaining_milliseconds(uint64_t now,
                                                  uint64_t maximum) const
{
    if (!pending()) return maximum;
    const uint64_t ready = deadline_ > retry_at_ ? deadline_ : retry_at_;
    const uint64_t remaining = ready > now ? ready - now : 0;
    return remaining < maximum ? remaining : maximum;
}

bool SessionStateSaver::due(uint64_t now) const
{
    return pending() && remaining_milliseconds(now, 1) == 0;
}

SessionSaveResult SessionStateSaver::save(const SessionState& settled,
                                          uint64_t now, bool immediately)
{
    if (!pending() || (!immediately && !due(now))) return SESSION_SAVE_UNCHANGED;
    try {
        observed_ = settled;
        position_pending_ = false;
        if (same_state(saved_, settled)) return SESSION_SAVE_UNCHANGED;
        if (!ensure_directory_tree(parent_directory(path_))) {
            retry_at_ = after_milliseconds(now, kSaveRetryMilliseconds);
            return SESSION_SAVE_FAILED;
        }
        // Keep the lock inode: unlinking it would let writers lock different files.
        SessionFileLock lock;
        const SessionSaveResult acquired = lock.acquire(path_);
        if (acquired != SESSION_SAVE_SAVED) {
            retry_at_ = after_milliseconds(now, acquired == SESSION_SAVE_BUSY
                ? kSaveDelayMilliseconds : kSaveRetryMilliseconds);
            return acquired;
        }
        SessionState latest;
        if (load_session_state(path_, latest) == SESSION_STATE_INVALID) {
            errno = EIO; // Do not replace a file that could not be read reliably.
            retry_at_ = after_milliseconds(now, kSaveRetryMilliseconds);
            return SESSION_SAVE_FAILED;
        }
        const std::string previous = serialize_session_state(latest);
        merge_local_changes(saved_, settled, latest);
        const bool identical = previous == serialize_session_state(latest);
        if (!identical && !save_session_state(path_, latest)) {
            retry_at_ = after_milliseconds(now, kSaveRetryMilliseconds);
            return SESSION_SAVE_FAILED;
        }
        saved_ = settled;
        retry_at_ = 0;
        return identical ? SESSION_SAVE_UNCHANGED : SESSION_SAVE_SAVED;
    } catch (const std::bad_alloc&) {
        errno = ENOMEM;
        retry_at_ = after_milliseconds(now, kSaveRetryMilliseconds);
        return SESSION_SAVE_FAILED;
    }
}

int validated_text_points(int points)
{ return points >= 8 && points <= 24 ? points : 12; }

SessionState default_session_state()
{
    SessionState state;
    state.width = kDefaultWidth;
    state.height = kDefaultHeight;
    state.x = 0;
    state.y = 0;
    state.has_position = false;
    state.position_is_outer = true;
    /* The current application starts analysis by default. */
    state.analysis_enabled = true;
    state.board_share = 0;
    state.figurines = false;
    state.notation_points = 12;
    state.engine_points = 12;
    state.piece_set_id = default_piece_set().id;
    state.board_theme_id = "classic";
    return state;
}

std::string session_state_path(const char* xdg_state_home, const char* home)
{
    if (xdg_state_home && xdg_state_home[0] == '/') {
        std::string base(xdg_state_home);
        while (base.size() > 1 && base[base.size() - 1] == '/')
            base.erase(base.size() - 1);
        return base == "/" ? base + "gambitree/state"
                            : base + "/gambitree/state";
    }
    if (home && home[0] == '/') {
        std::string base(home);
        while (base.size() > 1 && base[base.size() - 1] == '/')
            base.erase(base.size() - 1);
        return base + "/.local/state/gambitree/state";
    }
    return std::string();
}

bool parse_session_state(const std::string& text, SessionState& state)
{
    std::istringstream input;
    std::string line;
    bool valid = true;
    bool saw_x = false;
    bool saw_y = false;
    int version = 1;
    state = default_session_state();
    if (text.size() > kMaximumStateBytes)
        return false;
    input.str(text);
    while (std::getline(input, line)) {
        std::string::size_type equals;
        std::string key;
        std::string value;
        long integer;
        if (!line.empty() && line[line.size() - 1] == '\r')
            line.erase(line.size() - 1);
        equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        key = line.substr(0, equals);
        value = line.substr(equals + 1);
        if (key == "version") {
            if (value == "1")
                version = 1;
            else if (value == "2")
                version = 2;
            else {
                state = default_session_state();
                return false;
            }
        } else if (key == "width") {
            if (!parse_dimension(value, 1, state.width)) {
                state.width = kDefaultWidth;
                valid = false;
            }
        } else if (key == "height") {
            if (!parse_dimension(value, 1, state.height)) {
                state.height = kDefaultHeight;
                valid = false;
            }
        } else if (key == "x") {
            saw_x = parse_position(value, state.x);
            if (!saw_x) {
                state.x = 0;
                valid = false;
            }
        } else if (key == "y") {
            saw_y = parse_position(value, state.y);
            if (!saw_y) {
                state.y = 0;
                valid = false;
            }
        } else if (key == "analysis") {
            if (!parse_integer(value, integer) ||
                (integer != 0 && integer != 1)) {
                state.analysis_enabled = true;
                valid = false;
            } else {
                state.analysis_enabled = integer != 0;
            }
        } else if (key == "board_share") {
            if (!parse_integer(value, integer) || integer < 0 || integer > 9999) {
                state.board_share = 0;
                valid = false;
            } else state.board_share = static_cast<int>(integer);
        } else if (key == "figurines") {
            if (value != "0" && value != "1") {
                state.figurines = false;
                valid = false;
            } else state.figurines = value == "1";
        } else if (key == "notation_points" || key == "engine_points") {
            int points = 12;
            if (!parse_integer(value, integer) || integer < 8 || integer > 24)
                valid = false;
            else points = static_cast<int>(integer);
            if (key == "notation_points") state.notation_points = points;
            else state.engine_points = points;
        } else if (key == "pieces") {
            const PieceSetDescriptor* set = find_piece_set(value);
            if (set)
                state.piece_set_id = set->id;
            else {
                state.piece_set_id = default_piece_set().id;
                valid = false;
            }
        } else if (key == "board_theme") {
            if (valid_board_theme_id(value))
                state.board_theme_id = value;
            else {
                state.board_theme_id = "classic";
                valid = false;
            }
        }
    }
    state.has_position = saw_x && saw_y;
    state.position_is_outer = version >= 2;
    return valid;
}

std::string serialize_session_state(const SessionState& state)
{
    std::ostringstream output;
    int width = state.width;
    int height = state.height;
    if (width < 1)
        width = 1;
    if (width > 8192)
        width = 8192;
    if (height < 1)
        height = 1;
    if (height > 8192)
        height = 8192;
    output << "version=2\n"
           << "width=" << width << "\n"
           << "height=" << height << "\n";
    if (state.has_position)
        output << "x=" << state.x << "\n"
               << "y=" << state.y << "\n";
    output << "analysis=" << (state.analysis_enabled ? 1 : 0) << "\n"
           << "pieces=" << (find_piece_set(state.piece_set_id)
                              ? state.piece_set_id
                              : std::string(default_piece_set().id))
           << "\n"
           << "board_theme=" << (valid_board_theme_id(state.board_theme_id)
                                   ? state.board_theme_id : "classic") << "\n";
    output << "board_share=" << (state.board_share > 0 && state.board_share < 10000
                                      ? state.board_share : 0) << "\n"
           << "figurines=" << (state.figurines ? 1 : 0) << "\n"
           << "notation_points=" << validated_text_points(state.notation_points) << "\n"
           << "engine_points=" << validated_text_points(state.engine_points) << "\n";
    return output.str();
}

SessionStateLoadStatus load_session_state(const std::string& path,
                                          SessionState& state)
{
    try {
        struct stat info;
        std::ifstream input;
        std::string text;
        char buffer[4096];
        state = default_session_state();
        if (path.empty())
            return SESSION_STATE_ABSENT;
        if (stat(path.c_str(), &info) != 0) {
            return errno == ENOENT
                ? SESSION_STATE_ABSENT : SESSION_STATE_INVALID;
        }
        if (!S_ISREG(info.st_mode) || info.st_size < 0 ||
            static_cast<uint64_t>(info.st_size) > kMaximumStateBytes)
            return SESSION_STATE_INVALID;
        input.open(path.c_str(), std::ios::in | std::ios::binary);
        if (!input)
            return SESSION_STATE_INVALID;
        while (input) {
            input.read(buffer, sizeof(buffer));
            std::streamsize count = input.gcount();
            if (count > 0) {
                if (text.size() > kMaximumStateBytes -
                                  static_cast<std::size_t>(count))
                    return SESSION_STATE_INVALID;
                text.append(buffer, static_cast<std::size_t>(count));
            }
        }
        if (!input.eof())
            return SESSION_STATE_INVALID;
        return parse_session_state(text, state)
            ? SESSION_STATE_LOADED : SESSION_STATE_LOADED_WITH_INVALID_FIELDS;
    } catch (const std::bad_alloc&) {
        state = default_session_state();
        return SESSION_STATE_INVALID;
    }
}

bool save_session_state(const std::string& path, const SessionState& state)
{
    std::vector<char> name;
    int descriptor = -1;
    FILE* output = 0;
    bool created = false;
    try {
        std::string directory;
        std::string temporary;
        std::string contents;
        bool written;
        int saved_errno;
        if (path.empty())
            return false;
        directory = parent_directory(path);
        if (!ensure_directory_tree(directory))
            return false;
        contents = serialize_session_state(state);
        temporary = path + ".tmp.XXXXXX";
        name.assign(temporary.begin(), temporary.end());
        name.push_back('\0');
        descriptor = mkstemp(&name[0]);
        if (descriptor < 0)
            return false;
        created = true;
        if (fchmod(descriptor, 0600) != 0) {
            saved_errno = errno;
            close(descriptor);
            descriptor = -1;
            unlink(&name[0]);
            errno = saved_errno;
            return false;
        }
        output = fdopen(descriptor, "wb");
        if (!output) {
            saved_errno = errno;
            close(descriptor);
            descriptor = -1;
            unlink(&name[0]);
            errno = saved_errno;
            return false;
        }
        descriptor = -1;
        written = fwrite(contents.data(), 1, contents.size(), output) ==
                  contents.size();
        if (written && fflush(output) != 0)
            written = false;
        saved_errno = written ? 0 : errno;
        if (fclose(output) != 0) {
            output = 0;
            if (!saved_errno)
                saved_errno = errno;
            written = false;
        } else {
            output = 0;
        }
        if (written && rename(&name[0], path.c_str()) == 0)
            return true;
        if (written)
            saved_errno = errno;
        unlink(&name[0]);
        created = false;
        errno = saved_errno;
        return false;
    } catch (const std::bad_alloc&) {
        if (output)
            fclose(output);
        else if (descriptor >= 0)
            close(descriptor);
        if (created && !name.empty())
            unlink(&name[0]);
        errno = ENOMEM;
        return false;
    }
}

bool keep_window_visible(int& x, int& y, int width, int height,
                         int screen_x, int screen_y,
                         int screen_width, int screen_height)
{
    int64_t visible_x;
    int64_t visible_y;
    int64_t minimum_x;
    int64_t maximum_x;
    int64_t minimum_y;
    int64_t maximum_y;
    int64_t adjusted_x = x;
    int64_t adjusted_y = y;
    if (width <= 0 || height <= 0 || screen_width <= 0 || screen_height <= 0)
        return false;
    visible_x = width < 96 ? width : 96;
    visible_y = height < 96 ? height : 96;
    if (visible_x > screen_width)
        visible_x = screen_width;
    if (visible_y > screen_height)
        visible_y = screen_height;
    minimum_x = static_cast<int64_t>(screen_x) + visible_x - width;
    maximum_x = static_cast<int64_t>(screen_x) + screen_width - visible_x;
    minimum_y = static_cast<int64_t>(screen_y) + visible_y - height;
    maximum_y = static_cast<int64_t>(screen_y) + screen_height - visible_y;
    if (adjusted_x < minimum_x)
        adjusted_x = minimum_x;
    if (adjusted_x > maximum_x)
        adjusted_x = maximum_x;
    if (adjusted_y < minimum_y)
        adjusted_y = minimum_y;
    if (adjusted_y > maximum_y)
        adjusted_y = maximum_y;
    if (adjusted_x < INT_MIN || adjusted_x > INT_MAX ||
        adjusted_y < INT_MIN || adjusted_y > INT_MAX)
        return false;
    x = static_cast<int>(adjusted_x);
    y = static_cast<int>(adjusted_y);
    return true;
}

bool client_to_outer_position(int client_x, int client_y,
                              const WindowFrameExtents& extents,
                              int& outer_x, int& outer_y)
{
    int64_t x;
    int64_t y;
    if (extents.left < 0 || extents.right < 0 ||
        extents.top < 0 || extents.bottom < 0)
        return false;
    x = static_cast<int64_t>(client_x) - extents.left;
    y = static_cast<int64_t>(client_y) - extents.top;
    if (x < INT_MIN || x > INT_MAX || y < INT_MIN || y > INT_MAX)
        return false;
    outer_x = static_cast<int>(x);
    outer_y = static_cast<int>(y);
    return true;
}

bool outer_to_client_position(int outer_x, int outer_y,
                              const WindowFrameExtents& extents,
                              int& client_x, int& client_y)
{
    int64_t x;
    int64_t y;
    if (extents.left < 0 || extents.right < 0 ||
        extents.top < 0 || extents.bottom < 0)
        return false;
    x = static_cast<int64_t>(outer_x) + extents.left;
    y = static_cast<int64_t>(outer_y) + extents.top;
    if (x < INT_MIN || x > INT_MAX || y < INT_MIN || y > INT_MAX)
        return false;
    client_x = static_cast<int>(x);
    client_y = static_cast<int>(y);
    return true;
}

bool can_restore_session_position(const SessionState& state,
                                 bool have_frame_extents)
{
    return state.has_position && have_frame_extents;
}

bool initial_analysis_request_due(bool analysis_enabled,
                                  bool position_ready,
                                  bool request_already_sent)
{
    return analysis_enabled && position_ready && !request_already_sent;
}

}
