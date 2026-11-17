#ifndef SC_BANKSERV_LOAD_REPORT_H
#define SC_BANKSERV_LOAD_REPORT_H
// A bank's load report on an ACB (Bankserv) file sent host-to-host, read into its parts: FNB's
// FTPTCPIP.FNBLOAD.G<generation>V00 files, the answer to an uploaded debit order or payment file.
//
// A file holds one or more reports, each on pages that start with a form-feed line and a heading
// (company, run date, page, the report's title, effective date), and most end with "END OF REPORT
// <n>". Three kinds are known:
//
//   header_trailer  INPUT VALIDATE - HEADER/TRAILER RECORDS: the file's header and trailer fields,
//                   each with the bank's reason when it rejected it, the received, accepted and
//                   rejected counts and values, and DATA SET ACCEPTED or REJECTED. A real-time file's
//                   has one column of fields, and messages such as OVERALL FILE STATUS: REJECTED.
//   transactions    PAYMENTS AND COLLECTIONS TRANSACTIONS, one per transaction status (ACCEPTED ON
//                   LOAD, ACCEPTED VALIDATION, REJECTED VALIDATION, REJECTED ON LOAD, PENDING LIMIT
//                   AUTH, REJECT DUE TO LIMIT): the records listed, each with its reject code, and
//                   the totals by batch, service type, status, nominated account and customer service.
//   realtime        REAL-TIME PROCESSING TRANSACTIONS: the same for a real-time payment file, with
//                   rejected, accepted and total debits and credits.
//
// Nothing throws on what isn't known: a line the reader doesn't place is kept in unparsed, with its
// report and line number, so a caller can show it as it was. Text comes out as UTF-8 (other bytes
// read as Windows-1252); the carriage-control byte that starts each line is dropped. Amounts are
// exact: whole cents.
//
//     const auto report = sc::bankserv::read_load_report(file_contents);
//     if (report.outcome.rejected) ...;  // the whole file was rejected
//     for (const auto &section: report.reports)
//         for (const auto &record: section.records)
//             if (record.own_kind == "OWD" && !record.reject_code.empty()) ...;  // debit order record.own_id rejected

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sc::bankserv {
    enum class report_kind { header_trailer, transactions, realtime, other };

    // "header_trailer", "transactions", "realtime" or "other"
    [[nodiscard]] std::string_view to_string(report_kind kind);

    // A number of items and their value
    struct tally {
        long long count = 0;
        long long cents = 0;

        bool operator==(const tally &) const = default;
    };

    // A header and trailer field as the report shows it: the contents and the bank's reason when it
    // found one invalid (empty when it didn't); a real-time file's report has no trailer column.
    struct field {
        std::string name;                      // as printed, e.g. "USER GENERATION NUMBER"
        std::string header, header_reason;
        std::string trailer, trailer_reason;
    };

    // A line such as OVERALL FILE STATUS: REJECTED
    struct message {
        std::string label, text;
    };

    // A transaction listed in a transactions report
    struct record {
        long long sequence = 0;
        std::string name;                      // the homing account's name
        std::string account_type;              // ACC TYP, e.g. "1"
        std::string branch;                    // homing branch code
        std::string account;                   // homing account number
        std::string entry_class;               // ENT CLS, e.g. "44"
        char side = 'D';                       // 'D' debit, 'C' credit
        long long cents = 0;
        std::string action_date;               // YYYY-MM-DD
        std::string reference;                 // USER REFERENCE as printed
        std::string own_kind;                  // "OWD" or "OWC" when the reference carries such an id (OWD<n>, OWC<n>), else empty
        long long own_id = 0;                  // that id
        std::optional<long long> contra_batch; // the contra record's batch (reference ONE<batch>CONTRA)
        std::string reject_code, reject_description;
        std::vector<std::string> notes;        // other lines under it, e.g. a real-time file's errors
    };

    // A totals line: BATCH NO, SERVICE TYPE, STATUS, NOM ACC NO, CUST SERVICE or USER CODE, its
    // value, and what it counts ("" in a transactions report; "rejected", "accepted" or "total" in a
    // real-time one).
    struct summary {
        std::string level, key, counts;
        tally debit, credit;
    };

    struct report {
        report_kind kind = report_kind::other;
        std::string title;                     // e.g. "PAYMENTS AND COLLECTIONS TRANSACTIONS"
        std::string marker;                    // the form-feed line's numbers
        std::string company, bank, branch, service;
        std::string run_date, run_time, effective_date;
        int pages = 0;
        std::string number;                    // END OF REPORT <number>; empty when it has no end line
        int first_line = 0;                    // 1-based, in the file

        // header_trailer
        std::vector<field> fields;
        std::string generation, next_generation;
        std::optional<tally> received, accepted, rejected;
        std::string data_set;                  // e.g. "DATA SET ACCEPTED"
        std::vector<message> messages;

        // transactions and realtime
        std::string status, service_type, customer_service, user_code, batch, nominated_account, nominated_branch;
        std::vector<record> records;
        std::vector<summary> summaries;

        [[nodiscard]] const field *find_field(std::string_view name) const;
    };

    struct unparsed_line {
        int report = -1;                       // index into reports; -1 before the first
        int line = 0;                          // 1-based, in the file
        std::string text;
    };

    // What the file says about its batch
    struct outcome {
        bool realtime = false;                 // a real-time file's report
        bool rejected = false;                 // the data set, the file or a header field was rejected
        std::string user_code, generation;
        long long received = 0, accepted = 0, rejected_count = 0;
        long long adjusted = 0;                // OWD records with a reject code outside REJECTED VALIDATION (e.g. moved to the next business day)
    };

    struct load_report {
        std::vector<report> reports;
        std::vector<unparsed_line> unparsed;
        sc::bankserv::outcome outcome;
        int lines = 0;                         // in the file
    };

    // The file's contents, as received (any bytes)
    [[nodiscard]] load_report read_load_report(std::string_view contents);

    // Bytes as UTF-8: valid UTF-8 kept, other bytes read as Windows-1252
    [[nodiscard]] std::string to_utf8(std::string_view bytes);
}

#endif
