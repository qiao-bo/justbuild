#include <string>

#include "catch2/catch.hpp"
#include "gsl-lite/gsl-lite.hpp"
#include "src/buildtool/execution_api/local/local_ac.hpp"
#include "src/buildtool/file_system/file_system_manager.hpp"
#include "test/utils/hermeticity/local.hpp"

[[nodiscard]] static auto RunDummyExecution(gsl::not_null<LocalAC*> const& ac,
                                            bazel_re::Digest const& action_id,
                                            std::string const& seed) -> bool;

TEST_CASE_METHOD(HermeticLocalTestFixture,
                 "LocalAC: Single action, single result",
                 "[execution_api]") {
    LocalCAS cas{};
    LocalAC ac{&cas};

    auto action_id = ArtifactDigest::Create("action");
    CHECK(not ac.CachedResult(action_id));

    CHECK(RunDummyExecution(&ac, action_id, "result"));
    auto ac_result = ac.CachedResult(action_id);
    CHECK(ac_result);
}

TEST_CASE_METHOD(HermeticLocalTestFixture,
                 "LocalAC: Two different actions, two different results",
                 "[execution_api]") {
    LocalCAS cas{};
    LocalAC ac{&cas};

    auto action_id1 = ArtifactDigest::Create("action1");
    auto action_id2 = ArtifactDigest::Create("action2");
    CHECK(not ac.CachedResult(action_id1));
    CHECK(not ac.CachedResult(action_id2));

    std::string result_content1{};
    std::string result_content2{};

    CHECK(RunDummyExecution(&ac, action_id1, "result1"));
    auto ac_result1 = ac.CachedResult(action_id1);
    REQUIRE(ac_result1);
    CHECK(ac_result1->SerializeToString(&result_content1));

    CHECK(RunDummyExecution(&ac, action_id2, "result2"));
    auto ac_result2 = ac.CachedResult(action_id2);
    REQUIRE(ac_result2);
    CHECK(ac_result2->SerializeToString(&result_content2));

    // check different actions, different result
    CHECK(action_id1.hash() != action_id2.hash());
    CHECK(result_content1 != result_content2);
}

TEST_CASE_METHOD(HermeticLocalTestFixture,
                 "LocalAC: Two different actions, same two results",
                 "[execution_api]") {
    LocalCAS cas{};
    LocalAC ac{&cas};

    auto action_id1 = ArtifactDigest::Create("action1");
    auto action_id2 = ArtifactDigest::Create("action2");
    CHECK(not ac.CachedResult(action_id1));
    CHECK(not ac.CachedResult(action_id2));

    std::string result_content1{};
    std::string result_content2{};

    CHECK(RunDummyExecution(&ac, action_id1, "same result"));
    auto ac_result1 = ac.CachedResult(action_id1);
    REQUIRE(ac_result1);
    CHECK(ac_result1->SerializeToString(&result_content1));

    CHECK(RunDummyExecution(&ac, action_id2, "same result"));
    auto ac_result2 = ac.CachedResult(action_id2);
    REQUIRE(ac_result2);
    CHECK(ac_result2->SerializeToString(&result_content2));

    // check different actions, but same result
    CHECK(action_id1.hash() != action_id2.hash());
    CHECK(result_content1 == result_content2);
}

TEST_CASE_METHOD(HermeticLocalTestFixture,
                 "LocalAC: Same two actions, two differnet results",
                 "[execution_api]") {
    LocalCAS cas{};
    LocalAC ac{&cas};

    auto action_id = ArtifactDigest::Create("same action");
    CHECK(not ac.CachedResult(action_id));

    std::string result_content1{};
    std::string result_content2{};

    CHECK(RunDummyExecution(&ac, action_id, "result1"));
    auto ac_result1 = ac.CachedResult(action_id);
    REQUIRE(ac_result1);
    CHECK(ac_result1->SerializeToString(&result_content1));

    CHECK(RunDummyExecution(&ac, action_id, "result2"));  // updated
    auto ac_result2 = ac.CachedResult(action_id);
    REQUIRE(ac_result2);
    CHECK(ac_result2->SerializeToString(&result_content2));

    // check same actions, different cached result
    CHECK(result_content1 != result_content2);
}

auto RunDummyExecution(gsl::not_null<LocalAC*> const& ac,
                       bazel_re::Digest const& action_id,
                       std::string const& seed) -> bool {
    bazel_re::ActionResult result{};
    *result.add_output_files() = [&]() {
        bazel_re::OutputFile out{};
        out.set_path(seed);
        return out;
    }();
    return ac->StoreResult(action_id, result);
}
