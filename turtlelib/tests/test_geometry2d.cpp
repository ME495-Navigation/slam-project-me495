#include "turtlelib/geometry2d.hpp"

#include <random>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_random.hpp>
#include "turtlelib/test_utils.hpp"
#include <catch2/generators/catch_generators_adapters.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <sstream>
namespace turtlelib {

using Catch::Matchers::Equals;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE("Normalizing angles") {
  // double base_angle = 3* PI /2;
  REQUIRE_THAT(normalize_angle(PI), WithinRel(PI));
  REQUIRE_THAT(normalize_angle(-PI), WithinRel(PI));
  REQUIRE_THAT(normalize_angle(0), WithinRel(0.0));
  REQUIRE_THAT(normalize_angle(-PI / 4), WithinRel(-PI / 4));
  REQUIRE_THAT(normalize_angle(3 * PI / 2), WithinRel(-PI / 2));
  REQUIRE_THAT(normalize_angle(-5 * PI / 2), WithinRel(-PI / 2));

  constexpr double rad_step_size = 1e-5;

  // NOT using generator range, because the huge number of iterations
  // range generator is actually a lot a lot slower then double for loop.
  for (int circle_count = -4; circle_count < 4; ++circle_count) {
    // Sweep all angles within -PI to PI
    for (double base_angle = -PI + rad_step_size; base_angle < PI;
         base_angle += rad_step_size) {
      REQUIRE_THAT(normalize_angle(base_angle + circle_count * 2 * PI),
                   WithinAbs(base_angle, 1e-6));
    }
  }
};

TEST_CASE("Point2D tests", "[point2d]") {
  // Setup needed later
  Point2D point_a{2, 1};
  // Point2D point_b{3, 1};

  SECTION("OutStream") {

    std::stringstream ss;
    ss << point_a;
    REQUIRE_THAT(ss.str(), Equals("[2 1]"));
    point_a.x = 3.33;
    ss.str(std::string());
    ss << point_a;
    REQUIRE_THAT(ss.str(), Equals("[3.33 1]"));
    point_a.y = 10.678;
    ss.str(std::string());
    ss << point_a;
    REQUIRE_THAT(ss.str(), Equals("[3.33 10.678]"));
  }
  SECTION("InStream") {
    std::stringstream ss;
    ss << point_a;

    Point2D out_point;
    ss >> out_point;
    REQUIRE_THAT(out_point.x, WithinRel(point_a.x));
    REQUIRE_THAT(out_point.y, WithinRel(point_a.y));

    ss.str("10   20.12345");
    ss >> out_point;
    REQUIRE_THAT(out_point.x, WithinRel(10.0f));
    REQUIRE_THAT(out_point.y, WithinRel(20.12345));
  }
}

TEST_CASE("Vector2D stream tests", "[Vector2D]") {
  // Setup needed later
  Vector2D vector_a{2, 1};
  // Vector2D point_b{3, 1};

  SECTION("OutStream") {
    std::stringstream ss;
    ss << vector_a;
    REQUIRE_THAT(ss.str(), Equals("[2 1]"));
    vector_a.x = 3.33;
    ss.str(std::string());
    ss << vector_a;
    REQUIRE_THAT(ss.str(), Equals("[3.33 1]"));
    vector_a.y = 10.678;
    ss.str(std::string());
    ss << vector_a;
    REQUIRE_THAT(ss.str(), Equals("[3.33 10.678]"));
  }
  SECTION("InStream") {
    std::stringstream ss;
    ss << vector_a;

    Vector2D out_vec;
    ss >> out_vec;
    REQUIRE_THAT(out_vec.x, WithinRel(vector_a.x));
    REQUIRE_THAT(out_vec.y, WithinRel(vector_a.y));

    ss.str("10   20.12345");
    ss >> out_vec;
    REQUIRE_THAT(out_vec.x, WithinRel(10.0f));
    REQUIRE_THAT(out_vec.y, WithinRel(20.12345));
  }
}

TEST_CASE("Vector2D Normalize", "[Vector2D]") {

  auto scale_factor = GENERATE(
      Catch::Generators::take(5, Catch::Generators::random(-1e20, 1e20)));

  Vector2D vector_a{0.5547 * scale_factor, 0.8320 * scale_factor};
  auto normed_vector = vector_a.normalize();

  // Need to also be careful not to include 0s
  if (scale_factor < 0) {

    REQUIRE_THAT(normed_vector.x, WithinAbs(-0.5547, 1e-3));
    REQUIRE_THAT(normed_vector.y, WithinAbs(-0.8320, 1e-3));
  } else if (scale_factor > 0) {

    REQUIRE_THAT(normed_vector.x, WithinAbs(0.5547, 1e-3));
    REQUIRE_THAT(normed_vector.y, WithinAbs(0.8320, 1e-3));
  }
}

TEST_CASE("Point - Point") {
  Point2D point_tail{10, 10};
  Point2D point_head{15, 11};
  Vector2D vec = point_head - point_tail;
  REQUIRE_THAT(vec.x, WithinRel(5.0));
  REQUIRE_THAT(vec.y, WithinRel(1.0));
}

TEST_CASE("Point + Vector") {
  Point2D base_point{1, 3};
  Vector2D disp{10, 4};
  Point2D result = base_point + disp;
  REQUIRE_THAT(result.x, WithinRel(11.0));
  REQUIRE_THAT(result.y, WithinRel(7.0));
}

////==================================== 
////==================================== 
////       Vector2D tests
////==================================== 
////==================================== 

TEST_CASE("Vector Vector math", "[Vector2D],[TaskB8]") {

  SECTION("Vector add sub") {
    REQUIRE_THAT((Vector2D{1.0, 2.0} + Vector2D{3.0, 0.5}),
                 Vector2DWithinRel(Vector2D{4.0, 2.5}));
    REQUIRE_THAT((Vector2D{1.0, 2.0} - Vector2D{3.0, 0.5}),
                 Vector2DWithinRel(Vector2D{-2.0, 1.5}));

    Vector2D v1{-0.1, 0.5};
    Vector2D v2 = v1;
    v1 += Vector2D{0.2, -0.1};
    v2 -= Vector2D{0.2, -0.1};

    REQUIRE_THAT(v1 , Vector2DWithinRel( Vector2D{0.1, 0.4}));
    // This check can't be using simple equal. -0.3 is a number that actually is
    // not accurate in float, thus need the extra eps. hance the custom matcher
    REQUIRE_THAT(v2, Vector2DWithinRel(Vector2D{-0.3, 0.6}));
  }

  SECTION("Vector scaling") {
    double base_x = GENERATE(
        Catch::Generators::take(20, Catch::Generators::random(-1e10, 1e10)));
    double base_y = GENERATE(
        Catch::Generators::take(20, Catch::Generators::random(-1e10, 1e10)));
    double scale = GENERATE(
        Catch::Generators::take(20, Catch::Generators::random(-1e10, 1e10)));
    // The positive and negative is split because the
    Vector2D base_vec{base_x, base_y};
    Vector2D scaled_vec = base_vec * scale;

    INFO("vec " << base_vec);
    INFO("scale " << scale);
    INFO("scaled vec " << scaled_vec);
    INFO("scale abs " << std::abs(scale));
    REQUIRE_THAT(scaled_vec.magnitude(),
                 WithinRel(base_vec.magnitude() * std::abs(scale), 1e-10));
    // This is more of a direction check. see if negative scale flip sign.
    REQUIRE_THAT(
        scaled_vec.normalize().x,
        WithinRel(base_vec.normalize().x * std::copysign(1.0, scale), 1e-10));

    // Another form of multiply
    Vector2D self_inc_vec = base_vec;
    self_inc_vec *= scale;
    INFO("self_inc_vec " << self_inc_vec);

    REQUIRE_THAT(self_inc_vec.magnitude(),
                 WithinRel(base_vec.magnitude() * std::abs(scale), 1e-10));
    // This is more of a direction check. see if negative scale flip sign.
    REQUIRE_THAT(
        self_inc_vec.normalize().x,
        WithinRel(base_vec.normalize().x * std::copysign(1.0, scale), 1e-10));
  }

  SECTION("Vector dot") {
    // I didn't find good way of doing dot product, so just try a few value
    REQUIRE_THAT(dot({1.0, 2.0}, {3.0, 4.0}), WithinRel(11.0));
    REQUIRE_THAT(dot({0.1, 2.0}, {3.0, 1.5}), WithinRel(3.3));
    REQUIRE_THAT(dot({100.0, 500.0}, {10.0, 0.1}), WithinRel(1050.0));
    REQUIRE_THAT(dot({100.0, 0.0}, {0.0, 0.1}), WithinRel(0.0));
  }

  auto x_axis = Vector2D{1.0, 0.0};
  // https://en.cppreference.com/w/cpp/numeric/random/uniform_real_distribution
  std::random_device rd;
  std::mt19937 gen(rd()); // Standard mersenne_twister_engine seeded with rd()
  std::uniform_real_distribution<> dis(0.2, 1e6);

  SECTION("Vector angle") {
    // Point into right. basically the X axis.
    auto base_ang = GENERATE(
        Catch::Generators::take(20, Catch::Generators::random(-PI, PI)));
    auto flat_x = x_axis * dis(gen);

    auto v1 = Vector2D{cos(base_ang), sin(base_ang)} * dis(gen);
    REQUIRE_THAT(angle(flat_x, v1), WithinRel(base_ang, 1e-5));

    auto given_ang = GENERATE(Catch::Generators::take(
        50, Catch::Generators::random(-PI * 20, PI * 20)));
    auto normalized_given_ang = normalize_angle(given_ang);
    // The vector
    auto v2 = Vector2D{cos(given_ang + base_ang), sin(given_ang + base_ang)};

    REQUIRE_THAT(angle(v1, v2), WithinRel(normalized_given_ang, 1e-5));
  }
}

} // namespace turtlelib