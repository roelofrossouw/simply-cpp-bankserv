// sc-bankserv's example: a load report's parts, or with --summary one line per file (no names,
// accounts or references) for checking many files.
//
//     sc-bankserv-demo FILE...
//     sc-bankserv-demo --summary FILE...
#include <sc/bankserv/sc_bankserv.h>

#include <sc/core/params.h>

#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

namespace {
    std::string money(const long long cents) {
        std::ostringstream s;
        s << (cents < 0 ? "-" : "") << std::llabs(cents) / 100 << "." << (std::llabs(cents) % 100 < 10 ? "0" : "") << std::llabs(cents) % 100;
        return s.str();
    }

    void show(const sc::bankserv::load_report &file) {
        for (const auto &r: file.reports) {
            std::cout << sc::bankserv::to_string(r.kind) << ": " << r.title << " (page" << (r.pages > 1 ? "s " : " ") << r.pages
                      << ", run " << r.run_date << " " << r.run_time << ", effective " << r.effective_date << ")\n";
            for (const auto &f: r.fields) {
                std::cout << "  " << f.name << ": " << f.header;
                if (!f.header_reason.empty()) std::cout << " [" << f.header_reason << "]";
                if (!f.trailer.empty() || !f.trailer_reason.empty()) std::cout << " | " << f.trailer;
                if (!f.trailer_reason.empty()) std::cout << " [" << f.trailer_reason << "]";
                std::cout << "\n";
            }
            if (r.received) std::cout << "  received " << r.received->count << " (" << money(r.received->cents) << ")\n";
            if (r.accepted) std::cout << "  accepted " << r.accepted->count << " (" << money(r.accepted->cents) << ")\n";
            if (r.rejected) std::cout << "  rejected " << r.rejected->count << " (" << money(r.rejected->cents) << ")\n";
            if (!r.data_set.empty()) std::cout << "  " << r.data_set << "\n";
            for (const auto &m: r.messages) std::cout << "  " << m.label << ": " << m.text << "\n";
            if (!r.status.empty()) std::cout << "  status " << r.status << ", batch " << r.batch << "\n";
            for (const auto &rec: r.records) {
                std::cout << "  " << rec.sequence << " " << rec.name << " " << rec.side << " " << money(rec.cents) << " " << rec.action_date << " " << rec.reference;
                if (!rec.reject_code.empty()) std::cout << " [" << rec.reject_code << " " << rec.reject_description << "]";
                std::cout << "\n";
            }
            for (const auto &s: r.summaries)
                std::cout << "  " << s.level << " " << s.key << (s.counts.empty() ? "" : " " + s.counts) << ": debits " << s.debit.count << " ("
                          << money(s.debit.cents) << "), credits " << s.credit.count << " (" << money(s.credit.cents) << ")\n";
        }
        for (const auto &u: file.unparsed) std::cout << "unparsed, line " << u.line << ": " << u.text << "\n";
        const auto &o = file.outcome;
        std::cout << "outcome: " << (o.realtime ? "real-time, " : "") << (o.rejected ? "REJECTED, " : "") << "user code " << o.user_code << ", generation "
                  << o.generation << ", received " << o.received << ", accepted " << o.accepted << ", rejected " << o.rejected_count << ", adjusted "
                  << o.adjusted << "\n";
    }

    // One tab-separated line: counts and kinds only
    void summarise(const std::string &name, const sc::bankserv::load_report &file) {
        std::map<std::string, int> kinds, statuses;
        long long records = 0, rejected_records = 0, own = 0, listed_rejected = 0;
        for (const auto &r: file.reports) {
            ++kinds[std::string{sc::bankserv::to_string(r.kind)}];
            if (!r.status.empty()) ++statuses[r.status];
            for (const auto &rec: r.records) {
                ++records;
                if (!rec.reject_code.empty()) ++rejected_records;
                if (!rec.own_kind.empty()) ++own;
                if (r.status.rfind("REJECT", 0) == 0 && !rec.contra_batch) ++listed_rejected;
            }
        }
        const auto &o = file.outcome;
        std::cout << name << "\t" << file.lines << "\t" << file.reports.size() << "\t" << file.unparsed.size() << "\t";
        for (const auto &[k, n]: kinds) std::cout << k << "=" << n << ",";
        std::cout << "\t";
        for (const auto &[k, n]: statuses) std::cout << k << "=" << n << ",";
        std::cout << "\t" << o.realtime << "\t" << o.rejected << "\t" << o.user_code << "\t" << o.generation << "\t" << o.received << "\t" << o.accepted << "\t"
                  << o.rejected_count << "\t" << o.adjusted << "\t" << records << "\t" << rejected_records << "\t" << own << "\t" << listed_rejected << "\n";
    }
}

int main(int argc, char **argv) {
    const sc::params args{argc, argv, {
        {"description", "Reads Bankserv (ACB) load reports and prints their parts."},
        {"options", {{"summary", {{"short", "s"}, {"type", "boolean"}, {"help", "one tab-separated line of counts per file (no names, accounts or references)"}}}}},
        {"extra", {{"files", {{"required", true}, {"help", "load report files"}}}}}}};

    const bool summary = args.as<bool>("summary");
    int failed = 0;
    for (const auto &entry: args["files"]) {
        const std::string file = entry.get<std::string>();
        std::ifstream in{file, std::ios::binary};
        if (!in) {
            std::cerr << "can't read " << file << "\n";
            ++failed;
            continue;
        }
        std::ostringstream contents;
        contents << in.rdbuf();
        const auto report = sc::bankserv::read_load_report(contents.str());
        if (summary) summarise(file, report);
        else show(report);
    }
    return failed ? 1 : 0;
}
