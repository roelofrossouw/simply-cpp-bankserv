# simply-cpp-bankserv

Bankserv (South African ACB) files for simply-cpp, C++20, Apache-2.0. So far one: the bank's **load
report** on a debit order or payment file sent host-to-host (FNB's `FTPTCPIP.FNBLOAD.G####V00`), read
into its parts. It needs nothing beyond sc-core and the standard library.

```cpp
#include <sc/bankserv/sc_bankserv.h>

const auto report = sc::bankserv::read_load_report(contents);  // the file's bytes, as received
if (report.outcome.rejected) ...;                               // the data set or the file was rejected
for (const auto &section: report.reports)                       // one per report or transaction status
    for (const auto &record: section.records)
        if (record.own_kind == "OWD" && !record.reject_code.empty())
            ...;  // section.status, record.own_id, record.reject_code, record.reject_description
```

## What it reads

A file holds one or more reports on pages that start with a form-feed line (`0xBB 0x9F` and two
numbers) and a heading (company, bank, run date and time, page, the report's title, effective date);
most end with `END OF REPORT <n>`. Each line starts with a carriage-control byte (a space, `0x8D`,
`0x11` or `0x13`), dropped; the columns start after it. Text comes out as UTF-8, other bytes read as
Windows-1252. Amounts are whole cents.

| Report | `kind` | What comes out |
|---|---|---|
| INPUT VALIDATE - HEADER/TRAILER RECORDS | `header_trailer` | the header and trailer fields, each with the bank's reason when it rejected it (a real-time file's has one column); the generation and next generation numbers; received, accepted and rejected counts and values; the data set's result; messages such as `OVERALL FILE STATUS: REJECTED` |
| PAYMENTS AND COLLECTIONS TRANSACTIONS | `transactions` | one per transaction status (ACCEPTED ON LOAD, ACCEPTED VALIDATION, REJECTED VALIDATION, REJECTED ON LOAD, PENDING LIMIT AUTH, REJECT DUE TO LIMIT): service type, customer service, batch, nominated account and branch; the records listed (sequence, name, account type, branch, account, entry class and side, amount, action date, user reference, the OWD/OWC id in it, the contra's batch, reject code and description); the totals by batch, service type, status, nominated account and customer service |
| REAL-TIME PROCESSING TRANSACTIONS | `realtime` | the same for a real-time payment file, with the user code, and rejected, accepted and total debits and credits |

A new transaction status on a later page of the same report (the bank doesn't always end one before
the next) starts a section of its own. Nothing throws on what a file holds: a line the reader doesn't
place is kept in `unparsed` with its report and line number.

`outcome` gives what the file says about its batch: real-time
or not, rejected or not, the user code, the generation, received, accepted and rejected counts, and
`adjusted` (OWD records with a reject code outside REJECTED VALIDATION, such as ACTDTE ADJ NXT BUS/D).

Checked on 40,924 real load reports: none had a line it couldn't place, and its counts agreed with an
earlier reader's on 5,983 of 6,000 batches (the rest differ only in a rejected flag set elsewhere).

## The example

```
sc-bankserv-demo FILE...            the reports' parts
sc-bankserv-demo --summary FILE...  one line of counts per file (no names, accounts or references)
```

## PHP

sc-phplib's `SC\Bankserv::loadReport(string $contents)` and `SC\Bankserv::loadReportFile(string $path)`
give the same as arrays.

## Building

```
cmake -B build -S . && cmake --build build && ctest --test-dir build
```

The tests use made-up reports in the bank's layout (`tests/data`).
