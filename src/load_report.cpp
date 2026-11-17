#include <sc/bankserv/load_report.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <regex>

namespace sc::bankserv {
    namespace {
        // Windows-1252's characters for 0x80-0x9F (0: none, the byte stands for itself)
        constexpr std::array<char32_t, 32> windows_1252{
            0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
            0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178};

        void append(std::string &out, const char32_t c) {
            if (c < 0x80) {
                out += static_cast<char>(c);
            } else if (c < 0x800) {
                out += static_cast<char>(0xC0 | (c >> 6));
                out += static_cast<char>(0x80 | (c & 0x3F));
            } else if (c < 0x10000) {
                out += static_cast<char>(0xE0 | (c >> 12));
                out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (c & 0x3F));
            } else {
                out += static_cast<char>(0xF0 | (c >> 18));
                out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (c & 0x3F));
            }
        }

        // The length of a valid UTF-8 sequence at i, or 0
        std::size_t utf8_length(const std::string_view s, const std::size_t i) {
            const auto c = static_cast<unsigned char>(s[i]);
            std::size_t n = 0;
            char32_t least = 0;
            if (c >= 0xC2 && c <= 0xDF) n = 2, least = 0x80;
            else if (c >= 0xE0 && c <= 0xEF) n = 3, least = 0x800;
            else if (c >= 0xF0 && c <= 0xF4) n = 4, least = 0x10000;
            else return 0;
            if (i + n > s.size()) return 0;
            char32_t value = c & (0xFF >> (n + 1));
            for (std::size_t k = 1; k < n; ++k) {
                const auto d = static_cast<unsigned char>(s[i + k]);
                if ((d & 0xC0) != 0x80) return 0;
                value = (value << 6) | (d & 0x3F);
            }
            if (value < least || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) return 0;
            return n;
        }

        std::string trim(std::string_view s) {
            const auto first = s.find_first_not_of(" \t");
            if (first == std::string_view::npos) return {};
            return std::string{s.substr(first, s.find_last_not_of(" \t") - first + 1)};
        }

        // A field's text: trimmed, as UTF-8
        std::string text(const std::string_view s) { return to_utf8(trim(s)); }

        std::string_view slice(const std::string_view s, const std::size_t from, const std::size_t to = std::string_view::npos) {
            if (from >= s.size()) return {};
            return s.substr(from, to == std::string_view::npos ? std::string_view::npos : to - from);
        }

        // "1234.56" or "1234" (rands) as cents; "12345678901" when whole_cents (the header's values)
        long long cents_of(const std::string &s, const bool whole_cents = false) {
            long long units = 0, fraction = 0;
            int fraction_digits = -1;
            for (const char c: s) {
                if (c == '.') {
                    fraction_digits = 0;
                } else if (std::isdigit(static_cast<unsigned char>(c))) {
                    if (fraction_digits < 0) {
                        units = units * 10 + (c - '0');
                    } else if (fraction_digits < 2) {
                        fraction = fraction * 10 + (c - '0');
                        ++fraction_digits;
                    }
                }
            }
            if (whole_cents && fraction_digits < 0) return units;
            if (fraction_digits == 1) fraction *= 10;
            return units * 100 + fraction;
        }

        long long number_of(const std::string &s) {
            long long n = 0;
            for (const char c: s)
                if (std::isdigit(static_cast<unsigned char>(c))) n = n * 10 + (c - '0');
            return n;
        }

        // YYYY/MM/DD as YYYY-MM-DD
        std::string date_of(std::string s) {
            std::ranges::replace(s, '/', '-');
            return s;
        }

        bool starts_with(const std::string_view s, const std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

        // The header and trailer field names, longest first where one starts another
        const std::vector<std::string> field_names{
            "RECORD IDENTIFIER", "USER CODE", "CUSTOMER SERVICE NAME", "CREATION DATE", "PURGE DATE", "FIRST ACTION DATE",
            "LAST ACTION DATE", "FIRST SEQUENCE NUMBER", "LAST SEQUENCE NUMBER", "SEQUENCE NUMBER", "USER GENERATION NUMBER",
            "SERVICE TYPE", "NUMBER OF DEBIT RECORDS", "NUMBER OF CREDIT RECORDS", "NUMBER OF CONTRA RECORDS", "TOTAL DEBIT VALUE",
            "TOTAL CREDIT VALUE", "HASH TOTAL OF HOMING"};

        // Contents and reason: the reason is what starts at REJECTED (the bank writes them so)
        void split_reason(const std::string_view s, std::string &contents, std::string &reason) {
            const auto at = s.find("REJECTED");
            if (at == std::string_view::npos) {
                contents = text(s);
                reason.clear();
            } else {
                contents = text(s.substr(0, at));
                reason = text(s.substr(at));
            }
        }

        struct columns {
            std::size_t contents = 25, reason = std::string_view::npos;    // header (or the one column)
            std::size_t trailer = std::string_view::npos;                  // trailer contents
            std::size_t trailer_reason = std::string_view::npos;
        };

        const std::regex company_line{R"(^\s*(\S+)\s+COMPANY\s+(\S+)\s+(.*?)\s+RUN DATE\s+(\d{4}/\d{2}/\d{2})\s+(\d{2}:\d{2})\s+PAGE\s+(\d+)\s*$)"};
        const std::regex title_line{R"(^BRANCH\s+(\d+)\s+(.*?)\s{2,}(\S.*?)\s+EFF DATE\s+(\d{4}/\d{2}/\d{2})\s*$)"};
        const std::regex end_line{R"(^\s*END OF REPORT\s+(\d+)\s*$)"};
        const std::regex total_line{R"(^(?:TOTAL NUMBER OF|TRANSACTIONS)?\s*\d\.\s+(RECEIVED|ACCEPTED|REJECTED)\s+(\d+)\s+(?:TOTAL VALUE OF)?\s*\d\.\s+\1\s+(\d+)\s*(?:\*+\s*)?(.*?)\s*\**\s*$)"};
        const std::regex message_line{R"(^([A-Z][A-Z /.-]*[A-Z]):\s+(\S.*?)\s*$)"};
        const std::regex status_line{R"(^TRANSACTION STATUS\s*:\s*(.*?)(?:\s{2,}SERVICE TYPE\s*:\s*(.*?))?\s*$)"};
        const std::regex customer_line{R"(^(CUSTOMER SERVICE|CUSTOMER USER CODE)\s*:\s*(\S*)\s+BATCH NUMBER\s*:\s*(\S+)\s*$)"};
        const std::regex nominated_line{R"(^NOMINATED ACC NO\s*:\s*(\S*)\s+NOMINATED BRANCH\s*:\s*(\S*)\s*$)"};
        const std::regex record_tail{R"(^\s*(\S?)\s+(\d+)\s+(\d+)\s+(\d{2})([DC])\s+(\d+(?:\.\d+)?)\s+(\d{4}/\d{2}/\d{2})(?:\s+(.*?))?\s*$)"};
        const std::regex reject_line{R"(^\s*REJECT CODE:\s*(\S+)\s+DESCRIPTION:\s*(.*?)\s*$)"};
        // A reject code and its description at the record's right, without their labels
        const std::regex bare_reject_line{R"(^\s{60,}(\d{1,4})\s{2,}(\S.*?)\s*$)"};
        const std::regex note_line{R"(^\s+([A-Z][A-Z ]*:\s+.*?)\s*$)"};
        const std::regex summary_line{
            R"(^(BATCH NO|SERVICE TYPE|STATUS|NOM ACC NO|CUST SERVICE|USER CODE)\s*:\s*(.*?)\s+(VALUE DEBIT|REJ DR VALUE|ACC DR VALUE|TOT DR VALUE):\s*(\S+)\s+(?:NO DEBIT|REJ NO DR|ACC NO DR|TOT NO DR):\s*(\d+)\s+(?:VALUE CREDIT|REJ CR VALUE|ACC CR VALUE|TOT CR VALUE):\s*(\S+)\s+(?:NO CREDIT|REJ NO CR|ACC NO CR|TOT NO CR):\s*(\d+)\s*$)"};
        const std::regex own_reference{R"(OW([DC])(\d+))"};
        const std::regex contra_reference{R"(ONE(\d+)CONTRA)"};
        const std::regex next_generation{R"(NEXT GENERATION NUMBER\s*(\d+))"};

        bool is_control(const unsigned char c) { return c < 0x20 || (c >= 0x7F && c < 0xA0); }

        class reader {
        public:
            load_report out;

            void line(std::string_view raw, const int number) {
                if (!raw.empty() && raw.back() == '\r') raw.remove_suffix(1);
                // The form-feed line that starts a page: 0xBB 0x9F and the page's numbers
                if (raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xBB && static_cast<unsigned char>(raw[1]) == 0x9F) {
                    marker_ = text(raw.substr(2));
                    expect_heading_ = true;
                    return;
                }
                // The carriage-control byte (a space, 0x8D, 0x11, 0x13, ...): the columns start after it
                std::string_view s = raw;
                if (!s.empty() && (s[0] == ' ' || is_control(static_cast<unsigned char>(s[0])))) s.remove_prefix(1);
                while (!s.empty() && is_control(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
                if (trim(s).empty()) return;

                std::smatch m;
                const std::string line{s};
                number_ = number;
                if (std::regex_match(line, m, company_line)) return heading(m, number);
                if (std::regex_match(line, m, title_line)) return title(m, number);
                if (std::regex_match(line, m, end_line)) {
                    if (auto *r = current()) {
                        r->number = m[1];
                        ended_ = true;
                        return;
                    }
                }
                if (auto *r = current(); r && !ended_) {
                    bool placed = false;
                    switch (r->kind) {
                        case report_kind::header_trailer: placed = header_trailer(*r, line); break;
                        case report_kind::transactions:
                        case report_kind::realtime: placed = transactions(*r, line); break;
                        case report_kind::other: break;
                    }
                    if (placed) return;
                }
                out.unparsed.push_back({static_cast<int>(out.reports.size()) - 1, number, to_utf8(trim(s))});
            }

            void finish() {
                auto &o = out.outcome;
                for (const auto &r: out.reports) {
                    if (r.kind == report_kind::realtime) o.realtime = true;
                    if (r.kind == report_kind::header_trailer) {
                        if (r.data_set.find("REJECTED") != std::string::npos) o.rejected = true;
                        for (const auto &m: r.messages)
                            if (m.label.find("STATUS") != std::string::npos && m.text.find("REJECTED") != std::string::npos) o.rejected = true;
                        for (const auto &f: r.fields) {
                            if (!f.header_reason.empty() || !f.trailer_reason.empty()) o.rejected = true;
                            if (f.name == "USER CODE" && o.user_code.empty()) o.user_code = first_word(f.header);
                        }
                        if (o.generation.empty()) o.generation = r.generation;
                        if (r.received) o.received = r.received->count;
                        if (r.accepted) o.accepted = r.accepted->count;
                        if (r.rejected) o.rejected_count = r.rejected->count;
                    } else if (r.kind == report_kind::realtime || r.kind == report_kind::transactions) {
                        if (o.user_code.empty() && !r.user_code.empty()) o.user_code = r.user_code;
                        if (r.kind == report_kind::realtime && o.generation.empty()) o.generation = r.batch;
                        if (r.kind == report_kind::realtime) {
                            for (const auto &s: r.summaries) {
                                if (s.counts == "total") o.received = s.credit.count;
                                if (s.counts == "accepted") o.accepted = s.credit.count;
                                if (s.counts == "rejected") o.rejected_count = s.credit.count;
                            }
                        }
                        if (r.status != "REJECTED VALIDATION")
                            for (const auto &rec: r.records)
                                if (rec.own_kind == "OWD" && !rec.reject_code.empty()) ++o.adjusted;
                    }
                }
            }

        private:
            std::string marker_;
            int number_ = 0;
            bool expect_heading_ = false;
            bool ended_ = true;
            columns columns_;
            std::string pending_company_, pending_bank_, pending_run_date_, pending_run_time_;
            int pending_page_ = 0, pending_line_ = 0;

            report *current() { return out.reports.empty() ? nullptr : &out.reports.back(); }

            static std::string first_word(const std::string &s) { return s.substr(0, s.find(' ')); }

            void heading(const std::smatch &m, const int number) {
                pending_company_ = m[2];
                pending_bank_ = to_utf8(m[3].str());
                pending_run_date_ = date_of(m[4]);
                pending_run_time_ = m[5];
                pending_page_ = std::stoi(m[6]);
                pending_line_ = number;
                expect_heading_ = false;
            }

            void title(const std::smatch &m, const int number) {
                const std::string name = to_utf8(m[3].str());
                auto *r = current();
                // A further page of the report on hand: same title, a page after the first
                if (r && !ended_ && pending_page_ > 1 && r->title == name) {
                    ++r->pages;
                    return;
                }
                report next;
                next.title = name;
                next.kind = name == "INPUT VALIDATE - HEADER/TRAILER RECORDS"     ? report_kind::header_trailer
                            : name == "PAYMENTS AND COLLECTIONS TRANSACTIONS" ? report_kind::transactions
                            : name == "REAL-TIME PROCESSING TRANSACTIONS"     ? report_kind::realtime
                                                                              : report_kind::other;
                next.marker = marker_;
                next.company = pending_company_;
                next.bank = pending_bank_;
                next.run_date = pending_run_date_;
                next.run_time = pending_run_time_;
                next.branch = m[1];
                next.service = to_utf8(m[2].str());
                next.effective_date = date_of(m[4]);
                next.pages = 1;
                next.first_line = pending_line_ ? pending_line_ : number;
                out.reports.push_back(std::move(next));
                ended_ = false;
                columns_ = {};
                pending_line_ = 0;
            }

            bool header_trailer(report &r, const std::string &line) {
                std::smatch m;
                if (starts_with(trim(line), "F I E L D")) return true;
                if (const auto at = line.find("D E S C R I P T I O N"); at != std::string::npos) {
                    // CONTENTS and REASON IF INVALID once (a real-time file's) or twice (header, trailer)
                    std::vector<std::size_t> contents, reasons;
                    for (auto i = line.find("CONTENTS"); i != std::string::npos; i = line.find("CONTENTS", i + 1)) contents.push_back(i);
                    for (auto i = line.find("REASON IF INVALID"); i != std::string::npos; i = line.find("REASON IF INVALID", i + 1)) reasons.push_back(i);
                    columns_ = {};
                    if (!contents.empty()) columns_.contents = contents[0];
                    if (!reasons.empty()) columns_.reason = reasons[0];
                    if (contents.size() > 1) columns_.trailer = contents[1];
                    if (reasons.size() > 1) columns_.trailer_reason = reasons[1];
                    return true;
                }
                if (trim(line) == "ACCOUNT NUMBERS") return true; // HASH TOTAL OF HOMING's second line
                if (std::regex_match(line, m, total_line)) {
                    const tally t{std::stoll(m[2]), cents_of(m[3], true)};
                    if (m[1] == "RECEIVED") r.received = t;
                    else if (m[1] == "ACCEPTED") r.accepted = t;
                    else r.rejected = t;
                    if (m[4].matched && !m[4].str().empty()) r.data_set = to_utf8(m[4].str());
                    return true;
                }
                const auto name = std::ranges::find_if(field_names, [&](const std::string &n) {
                    return starts_with(line, n) && (line.size() == n.size() || line[n.size()] == ' ' || std::isdigit(static_cast<unsigned char>(line[n.size()])));
                });
                if (name != field_names.end()) {
                    field f;
                    f.name = *name;
                    // The contents start at the CONTENTS column (or right after a long name)
                    const std::size_t base = std::max(columns_.contents, name->size());
                    std::string rest{slice(line, base)};
                    // NEXT GENERATION NUMBER sits in the trailer's column of USER GENERATION NUMBER
                    std::smatch next;
                    std::string trailer_text;
                    if (std::regex_search(rest, next, next_generation)) {
                        r.next_generation = next[1];
                        trailer_text = next[1];
                        rest.erase(static_cast<std::size_t>(next.position(0)), static_cast<std::size_t>(next.length(0)));
                    }
                    if (columns_.trailer != std::string::npos) {
                        // Header half up to the trailer's column; a reason running past it ends at a gap of 3 spaces
                        const std::size_t split = columns_.trailer > base ? columns_.trailer - base : 0;
                        std::string header_part = rest.substr(0, std::min(split, rest.size()));
                        std::string trailer_part = rest.size() > split ? rest.substr(split) : std::string{};
                        if (const auto at = header_part.find("REJECTED"); at != std::string::npos && !trailer_part.empty() && trailer_part[0] != ' ' &&
                                                                          !header_part.empty() && header_part.back() != ' ') {
                            const std::string joined = header_part + trailer_part;
                            const auto gap = joined.find("   ", at);
                            header_part = joined.substr(0, gap);
                            trailer_part = gap == std::string::npos ? std::string{} : joined.substr(gap);
                        }
                        split_reason(header_part, f.header, f.header_reason);
                        split_reason(trailer_part, f.trailer, f.trailer_reason);
                        if (!trailer_text.empty() && f.trailer.empty()) f.trailer = trailer_text;
                    } else if (columns_.reason != std::string::npos && columns_.reason > base) {
                        const std::size_t split = columns_.reason - base;
                        f.header = text(slice(rest, 0, split));
                        f.header_reason = text(slice(rest, split));
                        if (const auto at = f.header.find("REJECTED"); at != std::string::npos) {
                            f.header_reason = text(f.header.substr(at) + " " + f.header_reason);
                            f.header = text(f.header.substr(0, at));
                        }
                        if (!trailer_text.empty()) f.trailer = trailer_text;
                    } else {
                        split_reason(rest, f.header, f.header_reason);
                    }
                    if (f.name == "USER GENERATION NUMBER") r.generation = first_word(f.header);
                    r.fields.push_back(std::move(f));
                    return true;
                }
                if (std::regex_match(line, m, message_line)) {
                    r.messages.push_back({to_utf8(m[1].str()), to_utf8(m[2].str())});
                    return true;
                }
                // A reason on a line of its own, under the field before it
                if (const auto t = trim(line); starts_with(t, "REJECTED") && !r.fields.empty()) {
                    auto &f = r.fields.back();
                    f.header_reason = text(f.header_reason.empty() ? t : f.header_reason + " " + t);
                    return true;
                }
                // The data set's result on a line of its own
                if (std::regex_match(line, m, std::regex{R"(^\s*\*+\s*(.*?)\s*\*+\s*$)"})) {
                    r.data_set = to_utf8(m[1].str());
                    return true;
                }
                return false;
            }

            bool transactions(report &r, const std::string &line) {
                std::smatch m;
                if (std::regex_match(line, m, status_line)) {
                    const std::string status = to_utf8(m[1].str());
                    // Another status on a later page of the same report: a section of its own
                    if (!r.status.empty() && r.status != status) {
                        report next;
                        next.kind = r.kind;
                        next.title = r.title;
                        next.marker = marker_;
                        next.company = r.company;
                        next.bank = r.bank;
                        next.branch = r.branch;
                        next.service = r.service;
                        next.run_date = r.run_date;
                        next.run_time = r.run_time;
                        next.effective_date = r.effective_date;
                        next.pages = 1;
                        next.first_line = number_;
                        out.reports.push_back(std::move(next));
                        return transactions(out.reports.back(), line);
                    }
                    r.status = status;
                    if (m[2].matched) r.service_type = to_utf8(m[2].str());
                    return true;
                }
                if (std::regex_match(line, m, customer_line)) {
                    if (m[1] == "CUSTOMER SERVICE") r.customer_service = m[2];
                    else r.user_code = m[2];
                    r.batch = m[3];
                    return true;
                }
                if (std::regex_match(line, m, nominated_line)) {
                    r.nominated_account = m[1];
                    r.nominated_branch = m[2];
                    return true;
                }
                if (starts_with(line, "SEQ NO") || (line.find("TYP BRANCH") != std::string::npos && line.find("CLS") != std::string::npos)) return true;
                if (std::regex_match(line, m, summary_line)) {
                    summary s;
                    s.level = m[1];
                    s.key = to_utf8(m[2].str());
                    const std::string kind = m[3];
                    s.counts = starts_with(kind, "REJ") ? "rejected" : starts_with(kind, "ACC") ? "accepted" : starts_with(kind, "TOT") ? "total" : "";
                    s.debit = {std::stoll(m[5]), cents_of(m[4])};
                    s.credit = {std::stoll(m[7]), cents_of(m[6])};
                    r.summaries.push_back(std::move(s));
                    return true;
                }
                if (std::regex_match(line, m, reject_line) || std::regex_match(line, m, bare_reject_line)) {
                    if (r.records.empty()) return false;
                    auto &rec = r.records.back();
                    // A record's first reject code is its own; a further one is kept as a note
                    if (rec.reject_code.empty()) {
                        rec.reject_code = m[1];
                        rec.reject_description = to_utf8(m[2].str());
                    } else {
                        rec.notes.push_back(to_utf8(trim(line)));
                    }
                    return true;
                }
                // A record: SEQ NO, the name in its 28 columns, then the rest by its spaces
                if (const auto seq = trim(slice(line, 0, 6));
                    line.size() > 40 && !seq.empty() && seq.find_first_not_of("0123456789") == std::string::npos) {
                    const std::string tail{slice(line, 36)};
                    if (std::regex_match(tail, m, record_tail)) {
                        record rec;
                        rec.sequence = number_of(std::string{slice(line, 0, 6)});
                        rec.name = text(slice(line, 6, 36));
                        rec.account_type = m[1];
                        rec.branch = m[2];
                        rec.account = m[3];
                        rec.entry_class = m[4];
                        rec.side = m[5].str()[0];
                        rec.cents = cents_of(m[6]);
                        rec.action_date = date_of(m[7]);
                        rec.reference = m[8].matched ? to_utf8(m[8].str()) : std::string{};
                        std::smatch own;
                        if (std::regex_search(rec.reference, own, own_reference)) {
                            rec.own_kind = std::string{"OW"} + own[1].str();
                            rec.own_id = std::stoll(own[2]);
                        }
                        if (std::regex_search(rec.reference, own, contra_reference)) rec.contra_batch = std::stoll(own[1]);
                        r.records.push_back(std::move(rec));
                        return true;
                    }
                }
                // Anything else under a record (a real-time file's errors): kept with it
                if (std::regex_match(line, m, note_line) && !r.records.empty()) {
                    r.records.back().notes.push_back(to_utf8(m[1].str()));
                    return true;
                }
                return false;
            }
        };
    }

    std::string_view to_string(const report_kind kind) {
        switch (kind) {
            case report_kind::header_trailer: return "header_trailer";
            case report_kind::transactions: return "transactions";
            case report_kind::realtime: return "realtime";
            case report_kind::other: break;
        }
        return "other";
    }

    const field *report::find_field(const std::string_view name) const {
        const auto it = std::ranges::find_if(fields, [&](const field &f) { return f.name == name; });
        return it == fields.end() ? nullptr : &*it;
    }

    std::string to_utf8(const std::string_view bytes) {
        std::string out;
        out.reserve(bytes.size());
        for (std::size_t i = 0; i < bytes.size();) {
            const auto c = static_cast<unsigned char>(bytes[i]);
            if (c < 0x80) {
                out += static_cast<char>(c);
                ++i;
            } else if (const auto n = utf8_length(bytes, i); n) {
                out.append(bytes.substr(i, n));
                i += n;
            } else {
                append(out, c >= 0x80 && c < 0xA0 && windows_1252[c - 0x80] ? windows_1252[c - 0x80] : char32_t{c});
                ++i;
            }
        }
        return out;
    }

    load_report read_load_report(const std::string_view contents) {
        reader r;
        int number = 0;
        std::size_t start = 0;
        while (start <= contents.size()) {
            const auto end = contents.find('\n', start);
            const auto line = contents.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
            ++number;
            if (!(end == std::string_view::npos && line.empty())) r.line(line, number);
            if (end == std::string_view::npos) break;
            start = end + 1;
        }
        r.out.lines = number;
        r.finish();
        return std::move(r.out);
    }
}
