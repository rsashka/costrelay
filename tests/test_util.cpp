#include "app/ServiceControl.h"
#include "util/NanoRub.h"
#include "util/Process.h"
#include "util/Strings.h"

#include <gtest/gtest.h>

using namespace proxy;

TEST(Util, NanoRubFormattingAndOverflow) {
    EXPECT_EQ(rubToNano(1.0L), 1'000'000'000LL);
    EXPECT_EQ(perMillionRubToPerTokenNano(100.0L), 100'000LL);
    EXPECT_EQ(formatRub(1'378'433'160LL), "1.378433");
    EXPECT_EQ(formatRubPerMillion(306'318LL, 3), "306.318");
    // Панельная сумма: две значащие цифры после запятой и группы разрядов.
    EXPECT_EQ(formatRubDisplay(296'051'452'000LL), "296.05");
    EXPECT_EQ(formatRubDisplay(12'345'678'900LL), "12.35");
    EXPECT_EQ(formatRubDisplay(70'000LL), "0.00007");
    EXPECT_EQ(formatRubDisplay(1'000'000'000'000LL), "1 000.00");
    EXPECT_EQ(safeMulTokens(2, 3'000'000'000LL), 6'000'000'000LL);
    EXPECT_EQ(safeMulTokens(0, 100), 0);
    EXPECT_GT(safeMulTokens(INT64_MAX, 2), 0);
}

TEST(Util, JoinStrings) {
    EXPECT_EQ(join({}, ", "), "");
    EXPECT_EQ(join({"one"}, ", "), "one");
    EXPECT_EQ(join({"one", "two", "three"}, " · "), "one · two · three");
}

TEST(Util, ProcessCapturesOutputAndExitCode) {
    const auto ok = runProcess({"/bin/echo", "hello-process"});
    EXPECT_TRUE(ok.ok());
    EXPECT_NE(ok.output.find("hello-process"), std::string::npos);

    const auto failed = runProcess({"/bin/false"});
    EXPECT_FALSE(failed.ok());
    EXPECT_NE(failed.exit_code, 0);

    const auto missing = runProcess({"/definitely/not/a/binary"});
    EXPECT_FALSE(missing.ok());
    EXPECT_FALSE(missing.output.empty());
}

TEST(Util, ServiceTargetsAndRestartCommand) {
    const auto* dsh = findServiceTarget("dsh");
    ASSERT_NE(dsh, nullptr);
    EXPECT_STREQ(dsh->unit, "dsh-web.service");
    EXPECT_EQ(findServiceTarget("unknown"), nullptr);

    const auto command = restartServiceCommand("strata.service");
    ASSERT_EQ(command.size(), 6u);
    EXPECT_EQ(command[0], "/usr/bin/sudo");
    EXPECT_EQ(command[1], "-n");
    EXPECT_EQ(command[2], "/usr/bin/systemctl");
    EXPECT_EQ(command[3], "restart");
    EXPECT_EQ(command[4], "--no-block");
    EXPECT_EQ(command[5], "strata.service");
}

// Правило идентификатора запроса одно: заголовок клиента и имя файла тела
// используют его же, поэтому длина и набор символов проверяются на краях.
TEST(Util, RequestIdHasOneRule) {
    EXPECT_TRUE(isRequestId("abc12345"));
    EXPECT_TRUE(isRequestId(std::string(64, 'a')));
    EXPECT_TRUE(isRequestId("a-b_c-123"));
    EXPECT_FALSE(isRequestId("abc1234"));             // короче 8
    EXPECT_FALSE(isRequestId(std::string(65, 'a')));  // длиннее 64
    EXPECT_FALSE(isRequestId("abc.12345"));           // точка недопустима
    EXPECT_FALSE(isRequestId("abc/12345"));
    EXPECT_FALSE(isRequestId("abc 12345"));
    EXPECT_FALSE(isRequestId(""));
}
