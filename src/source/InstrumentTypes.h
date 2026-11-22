#pragma once

#include "model/Types.h"

#include <string>

// The providers' own types of instruments, mapped onto the one vocabulary the program uses (`InstrumentKind`). Each
// adapter keeps the provider's type beside it (`Instrument::type`) and sets the kind from here, so that a certificate
// is a certificate whoever lists it. A type that is not known is Other.

// Nordnet's `instrument_type`: UETF and KETF are ETFs, PTC are certificates.
InstrumentKind kindOfNordnetType(const std::string & type);

// Yahoo's `instrumentType` (in a chart's meta): EQUITY, ETF, INDEX, FUTURE, CURRENCY, ... A future is taken as a
// commodity, which is what the lists hold; an index future would be wrong.
InstrumentKind kindOfYahooType(const std::string & type);
