// sc::bankserv::read_load_report on made-up load reports in the bank's layout (tests/data):
//   debit-orders.txt       a debit order file: header and trailer, one record rejected on validation,
//                          the rest accepted on load (one moved to the next business day), spaces as
//                          carriage control
//   rejected-file.txt      the other style (0x8D, 0x11, 0x13 carriage control): a reason glued to a
//                          date, the data set rejected, a name in Windows-1252, a second page, and a
//                          line the reader doesn't know
//   realtime.txt           a real-time payment file accepted
//   realtime-rejected.txt  one rejected outright: one column of fields, messages, an error note
//   two-statuses.txt       REJECTED ON LOAD running into ACCEPTED ON LOAD on the next page with no end
//                          line between, and a reject code without its labels
// and the edge cases: nothing, lines before a report, UTF-8, and no end line.
#include <sc/bankserv/sc_bankserv.h>
#include <sc_test.h>

#include <fstream>
#include <sstream>
#include <string>

namespace {
    std::string read(const std::string &name) {
        std::ifstream in{std::string{SC_BANKSERV_TEST_DATA} + "/" + name, std::ios::binary};
        std::ostringstream s;
        s << in.rdbuf();
        return s.str();
    }
}

int main() {
    using namespace sc::bankserv;
    using std::string;

    SECTION("A debit order file: header and trailer fields, totals, two transaction statuses");
    {
        const auto file = read_load_report(read("debit-orders.txt"));
        CHECK(file.unparsed.empty());
        for (const auto &u: file.unparsed) std::cerr << "unparsed " << u.line << ": " << u.text << "\n";
        CHECK_EQ(file.reports.size(), std::size_t{3});
        const auto &ht = file.reports.at(0);
        CHECK(ht.kind == report_kind::header_trailer);
        CHECK_EQ(ht.title, string{"INPUT VALIDATE - HEADER/TRAILER RECORDS"});
        CHECK_EQ(ht.run_date, string{"2026-03-02"});
        CHECK_EQ(ht.run_time, string{"09:15"});
        CHECK_EQ(ht.effective_date, string{"2026-03-02"});
        CHECK_EQ(ht.company, string{"12345"});
        CHECK_EQ(ht.branch, string{"255"});
        CHECK_EQ(ht.service, string{"REMOTE BANKING SERVICE"});
        CHECK_EQ(ht.marker, string{"12345     00150560"});
        CHECK_EQ(ht.number, string{"00101"});
        CHECK_EQ(ht.fields.size(), std::size_t{18});
        const auto *user = ht.find_field("USER CODE");
        CHECK(user && user->header == "K999" && user->trailer == "K999" && user->header_reason.empty());
        const auto *name = ht.find_field("CUSTOMER SERVICE NAME");
        CHECK(name && name->header == "EXAMPLE CO   K999" && name->trailer == "N/A");
        const auto *debit = ht.find_field("TOTAL DEBIT VALUE");
        CHECK(debit && debit->header == "N/A" && debit->trailer == "00000250000");
        CHECK_EQ(ht.generation, string{"0123"});
        CHECK_EQ(ht.next_generation, string{"0124"});
        CHECK(ht.received && *ht.received == (tally{3, 250000}));
        CHECK(ht.accepted && *ht.accepted == (tally{2, 150000}));
        CHECK(ht.rejected && *ht.rejected == (tally{1, 100000}));
        CHECK_EQ(ht.data_set, string{"DATA SET ACCEPTED"});

        const auto &rejected = file.reports.at(1);
        CHECK(rejected.kind == report_kind::transactions);
        CHECK_EQ(rejected.status, string{"REJECTED VALIDATION"});
        CHECK_EQ(rejected.service_type, string{"SAMEDAY"});
        CHECK_EQ(rejected.customer_service, string{"ZPC00000000000000001"});
        CHECK_EQ(rejected.batch, string{"0123"});
        CHECK_EQ(rejected.nominated_account, string{"00000000062000000001"});
        CHECK_EQ(rejected.nominated_branch, string{"250655"});
        CHECK_EQ(rejected.records.size(), std::size_t{1});
        const auto &r = rejected.records.at(0);
        CHECK_EQ(r.sequence, 2LL);
        CHECK_EQ(r.name, string{"J DOE"});
        CHECK_EQ(r.account_type, string{"1"});
        CHECK_EQ(r.branch, string{"250655"});
        CHECK_EQ(r.account, string{"62000000002"});
        CHECK_EQ(r.entry_class, string{"44"});
        CHECK_EQ(r.side, 'D');
        CHECK_EQ(r.cents, 100000LL);
        CHECK_EQ(r.action_date, string{"2026-03-02"});
        CHECK_EQ(r.reference, string{"EXAMPLE CO  OWD1001"});
        CHECK_EQ(r.own_kind, string{"OWD"});
        CHECK_EQ(r.own_id, 1001LL);
        CHECK_EQ(r.reject_code, string{"152"});
        CHECK_EQ(r.reject_description, string{"HOM ACCT TYPE INVAL"});
        CHECK_EQ(rejected.summaries.size(), std::size_t{5});
        CHECK_EQ(rejected.summaries.at(2).level, string{"STATUS"});
        CHECK_EQ(rejected.summaries.at(2).key, string{"REJECTED VALIDATION"});
        CHECK(rejected.summaries.at(2).debit == (tally{1, 100000}));
        CHECK(rejected.summaries.at(2).credit == (tally{0, 0}));

        const auto &accepted = file.reports.at(2);
        CHECK_EQ(accepted.status, string{"ACCEPTED ON LOAD"});
        CHECK_EQ(accepted.records.size(), std::size_t{2});
        CHECK_EQ(accepted.records.at(0).reject_code, string{"44"});
        CHECK(!accepted.records.at(0).contra_batch);
        CHECK(accepted.records.at(1).contra_batch && *accepted.records.at(1).contra_batch == 123);
        CHECK_EQ(accepted.records.at(1).side, 'C');
        CHECK(accepted.records.at(1).own_kind.empty());

        const auto &o = file.outcome;
        CHECK(!o.realtime);
        CHECK(!o.rejected);
        CHECK_EQ(o.user_code, string{"K999"});
        CHECK_EQ(o.generation, string{"0123"});
        CHECK_EQ(o.received, 3LL);
        CHECK_EQ(o.accepted, 2LL);
        CHECK_EQ(o.rejected_count, 1LL);
        CHECK_EQ(o.adjusted, 1LL); // OWD1002 moved to the next business day; OWD1001 was rejected on validation
    }

    SECTION("The other style: control bytes, a glued reason, a rejected data set, Windows-1252, two pages");
    {
        const auto file = read_load_report(read("rejected-file.txt"));
        CHECK_EQ(file.reports.size(), std::size_t{2});
        const auto &ht = file.reports.at(0);
        const auto *first = ht.find_field("FIRST ACTION DATE");
        CHECK(first && first->header == "2026/03/02");
        CHECK(first && first->header_reason == "REJECTED : 6 ACTION DATE < CREATION DATE 2026/03/01");
        CHECK(first && first->trailer == "2026/03/02");
        CHECK_EQ(ht.next_generation, string{"0201"});
        CHECK_EQ(ht.data_set, string{"DATA SET REJECTED"});
        CHECK(ht.rejected && ht.rejected->count == 2);
        const auto &t = file.reports.at(1);
        CHECK_EQ(t.status, string{"REJECTED ON LOAD"});
        CHECK_EQ(t.pages, 2);
        CHECK_EQ(t.records.size(), std::size_t{2});
        CHECK_EQ(t.records.at(0).name, string{"CAF\xc3\x89 OWNER"}); // 0xC9 in Windows-1252
        CHECK_EQ(t.records.at(1).branch, string{"051001"});
        CHECK_EQ(t.records.at(1).reject_code, string{"267"});
        CHECK(t.summaries.at(0).credit == (tally{0, 0})); // "0" without cents
        CHECK_EQ(file.unparsed.size(), std::size_t{1});
        CHECK_EQ(file.unparsed.at(0).text, string{"SOMETHING THE READER HAS NOT SEEN"});
        CHECK_EQ(file.unparsed.at(0).report, 1);
        CHECK(file.outcome.rejected);
        CHECK_EQ(file.outcome.adjusted, 2LL); // rejected on load, not on validation: f.h2h_loadreport counts them so
    }

    SECTION("A real-time payment file accepted");
    {
        const auto file = read_load_report(read("realtime.txt"));
        CHECK(file.unparsed.empty());
        for (const auto &u: file.unparsed) std::cerr << "unparsed " << u.line << ": " << u.text << "\n";
        CHECK_EQ(file.reports.size(), std::size_t{1});
        const auto &r = file.reports.at(0);
        CHECK(r.kind == report_kind::realtime);
        CHECK_EQ(r.status, string{"ACCEPTED"});
        CHECK(r.service_type.empty());
        CHECK_EQ(r.user_code, string{"K999"});
        CHECK_EQ(r.batch, string{"0042"});
        CHECK(r.number.empty());
        CHECK_EQ(r.records.size(), std::size_t{2});
        CHECK_EQ(r.records.at(1).own_kind, string{"OWC"});
        CHECK_EQ(r.records.at(1).own_id, 3001LL);
        CHECK_EQ(r.summaries.size(), std::size_t{4});
        CHECK_EQ(r.summaries.at(1).counts, string{"rejected"});
        CHECK_EQ(r.summaries.at(3).counts, string{"total"});
        const auto &o = file.outcome;
        CHECK(o.realtime);
        CHECK(!o.rejected);
        CHECK_EQ(o.user_code, string{"K999"});
        CHECK_EQ(o.generation, string{"0042"});
        CHECK_EQ(o.received, 1LL);
        CHECK_EQ(o.accepted, 1LL);
        CHECK_EQ(o.rejected_count, 0LL);
    }

    SECTION("A real-time file rejected outright: one column of fields, messages, an error under a record");
    {
        const auto file = read_load_report(read("realtime-rejected.txt"));
        CHECK(file.unparsed.empty());
        for (const auto &u: file.unparsed) std::cerr << "unparsed " << u.line << ": " << u.text << "\n";
        CHECK_EQ(file.reports.size(), std::size_t{2});
        const auto &ht = file.reports.at(0);
        CHECK(ht.kind == report_kind::header_trailer);
        const auto *user = ht.find_field("USER CODE");
        CHECK(user && user->header == "M999" && user->header_reason == "USER CODE NOT FOUND ON CUST FILE" && user->trailer.empty());
        const auto *sequence = ht.find_field("SEQUENCE NUMBER");
        CHECK(sequence && sequence->header == "000001");
        CHECK_EQ(ht.next_generation, string{"0043"});
        CHECK_EQ(ht.messages.size(), std::size_t{2});
        CHECK_EQ(ht.messages.at(1).label, string{"OVERALL FILE STATUS"});
        CHECK_EQ(ht.messages.at(1).text, string{"REJECTED"});
        const auto &t = file.reports.at(1);
        CHECK_EQ(t.records.size(), std::size_t{1});
        CHECK_EQ(t.records.at(0).notes.size(), std::size_t{1});
        CHECK_EQ(t.records.at(0).notes.at(0), string{"ERROR:    12    34 INVALID REFERENCE VALIDATION"});
        CHECK(t.records.at(0).own_kind.empty());
        CHECK(file.outcome.realtime);
        CHECK(file.outcome.rejected);
        CHECK_EQ(file.outcome.user_code, string{"M999"});
        CHECK_EQ(file.outcome.generation, string{"0042"});
        CHECK_EQ(file.outcome.rejected_count, 1LL);
    }

    SECTION("Two statuses in one report (no end line between): two sections; a reject code without its labels");
    {
        const auto file = read_load_report(read("two-statuses.txt"));
        CHECK(file.unparsed.empty());
        for (const auto &u: file.unparsed) std::cerr << "unparsed " << u.line << ": " << u.text << "\n";
        CHECK_EQ(file.reports.size(), std::size_t{2});
        CHECK_EQ(file.reports.at(0).status, string{"REJECTED ON LOAD"});
        CHECK_EQ(file.reports.at(0).records.size(), std::size_t{1});
        CHECK_EQ(file.reports.at(0).records.at(0).reject_code, string{"070"});
        CHECK_EQ(file.reports.at(0).records.at(0).reject_description, string{"HOME ACC NO. INVALID"});
        CHECK_EQ(file.reports.at(0).summaries.size(), std::size_t{1});
        CHECK(file.reports.at(0).number.empty());
        CHECK_EQ(file.reports.at(1).status, string{"ACCEPTED ON LOAD"});
        CHECK_EQ(file.reports.at(1).batch, string{"0300"});
        CHECK_EQ(file.reports.at(1).records.size(), std::size_t{1});
        CHECK_EQ(file.reports.at(1).number, string{"00103"});
    }

    SECTION("Nothing, lines before a report, carriage returns, UTF-8 kept");
    {
        const auto none = read_load_report("");
        CHECK(none.reports.empty());
        CHECK(none.unparsed.empty());
        const auto before = read_load_report("garbage first\r\n\r\n" + read("realtime.txt"));
        CHECK_EQ(before.unparsed.size(), std::size_t{1});
        CHECK_EQ(before.unparsed.at(0).report, -1);
        CHECK_EQ(before.unparsed.at(0).line, 1);
        CHECK_EQ(before.reports.size(), std::size_t{1});
        CHECK_EQ(to_utf8("caf\xc3\xa9"), string{"caf\xc3\xa9"});  // valid UTF-8 kept
        CHECK_EQ(to_utf8("caf\xe9"), string{"caf\xc3\xa9"});      // Windows-1252
        CHECK_EQ(to_utf8("\x80"), string{"\xe2\x82\xac"});        // the euro sign
        CHECK_EQ(to_string(report_kind::header_trailer), std::string_view{"header_trailer"});
    }
}
