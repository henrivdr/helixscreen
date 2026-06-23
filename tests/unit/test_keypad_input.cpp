// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "keypad_input.h"

#include <cstring>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::ui::KeypadInput;

TEST_CASE("KeypadInput digit entry", "[keypad]") {
    KeypadInput kp;

    SECTION("appends digits") {
        REQUIRE(kp.append_digit(1));
        REQUIRE(kp.append_digit(2));
        REQUIRE(kp.append_digit(3));
        CHECK(std::string(kp.buf) == "123");
    }

    SECTION("limits to 3 digits without decimal") {
        kp.append_digit(1);
        kp.append_digit(2);
        kp.append_digit(3);
        CHECK_FALSE(kp.append_digit(4));
        CHECK(std::string(kp.buf) == "123");
    }

    SECTION("allows 5 digits with decimal") {
        kp.append_digit(2);
        kp.append_digit(5);
        kp.append_digit(0);
        kp.append_dot();
        kp.append_digit(7);
        kp.append_digit(5);
        CHECK(std::string(kp.buf) == "250.75");
        CHECK_FALSE(kp.append_digit(9));
    }

    SECTION("rejects invalid digit values") {
        CHECK_FALSE(kp.append_digit(-1));
        CHECK_FALSE(kp.append_digit(10));
        CHECK(std::string(kp.buf) == "");
    }

    SECTION("zero works normally") {
        REQUIRE(kp.append_digit(0));
        CHECK(std::string(kp.buf) == "0");
    }
}

TEST_CASE("KeypadInput decimal point", "[keypad]") {
    KeypadInput kp;

    SECTION("appends dot") {
        kp.append_digit(1);
        REQUIRE(kp.append_dot());
        CHECK(std::string(kp.buf) == "1.");
    }

    SECTION("only allows one dot") {
        kp.append_digit(1);
        kp.append_dot();
        CHECK_FALSE(kp.append_dot());
        CHECK(std::string(kp.buf) == "1.");
    }

    SECTION("dot as first character") {
        REQUIRE(kp.append_dot());
        kp.append_digit(5);
        CHECK(std::string(kp.buf) == ".5");
        CHECK(kp.value() == Catch::Approx(0.5f));
    }

    SECTION("digits after dot count toward 5-digit limit") {
        // 12.345 = 5 digits + dot
        kp.append_digit(1);
        kp.append_digit(2);
        kp.append_dot();
        kp.append_digit(3);
        kp.append_digit(4);
        kp.append_digit(5);
        CHECK(std::string(kp.buf) == "12.345");
        CHECK_FALSE(kp.append_digit(6));
    }
}

TEST_CASE("KeypadInput backspace", "[keypad]") {
    KeypadInput kp;

    SECTION("removes last character") {
        kp.append_digit(1);
        kp.append_digit(2);
        kp.append_digit(3);
        REQUIRE(kp.backspace());
        CHECK(std::string(kp.buf) == "12");
    }

    SECTION("removes dot") {
        kp.append_digit(1);
        kp.append_dot();
        kp.backspace();
        CHECK(std::string(kp.buf) == "1");
        CHECK_FALSE(kp.has_dot());
    }

    SECTION("backspace on empty returns false") {
        CHECK_FALSE(kp.backspace());
    }

    SECTION("can re-add dot after backspacing it") {
        kp.append_digit(1);
        kp.append_dot();
        kp.backspace();
        REQUIRE(kp.append_dot());
        CHECK(std::string(kp.buf) == "1.");
    }

    SECTION("backspace all then re-enter") {
        kp.append_digit(5);
        kp.backspace();
        CHECK(std::string(kp.buf) == "");
        REQUIRE(kp.append_digit(9));
        CHECK(std::string(kp.buf) == "9");
    }
}

TEST_CASE("KeypadInput value parsing", "[keypad]") {
    KeypadInput kp;

    SECTION("empty buffer is 0") {
        CHECK(kp.value() == 0.0f);
    }

    SECTION("integer value") {
        kp.append_digit(2);
        kp.append_digit(5);
        kp.append_digit(0);
        CHECK(kp.value() == Catch::Approx(250.0f));
    }

    SECTION("decimal value") {
        kp.append_digit(1);
        kp.append_dot();
        kp.append_digit(5);
        CHECK(kp.value() == Catch::Approx(1.5f));
    }

    SECTION("trailing dot parses as integer") {
        kp.append_digit(4);
        kp.append_digit(2);
        kp.append_dot();
        CHECK(kp.value() == Catch::Approx(42.0f));
    }
}

TEST_CASE("KeypadInput clear", "[keypad]") {
    KeypadInput kp;
    kp.append_digit(1);
    kp.append_dot();
    kp.append_digit(5);

    kp.clear();
    CHECK(std::string(kp.buf) == "");
    CHECK(kp.value() == 0.0f);
    CHECK_FALSE(kp.has_dot());
}

TEST_CASE("KeypadInput digit limit transitions", "[keypad]") {
    KeypadInput kp;

    SECTION("adding dot after 3 digits allows more digits") {
        kp.append_digit(1);
        kp.append_digit(2);
        kp.append_digit(3);
        CHECK_FALSE(kp.append_digit(4)); // blocked at 3
        REQUIRE(kp.append_dot());
        REQUIRE(kp.append_digit(4)); // now allowed (5 digit limit)
        REQUIRE(kp.append_digit(5));
        CHECK(std::string(kp.buf) == "123.45");
        CHECK_FALSE(kp.append_digit(6)); // blocked at 5
    }

    SECTION("backspace below limit allows re-entry") {
        kp.append_digit(1);
        kp.append_digit(2);
        kp.append_digit(3);
        kp.backspace();
        REQUIRE(kp.append_digit(9));
        CHECK(std::string(kp.buf) == "129");
    }
}
