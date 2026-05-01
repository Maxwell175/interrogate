#include "advanced_features.h"

Shape::Shape() : _id(0) {}
Shape::~Shape() {}
float Shape::area() const { return 0.0f; }
std::string Shape::type_name() const { return "shape"; }
int Shape::get_id() const { return _id; }
void Shape::set_id(int id) { _id = id; }

Circle::Circle(float radius) : _radius(radius) {}
float Circle::get_radius() const { return _radius; }
void Circle::set_radius(float r) { _radius = r; }
float Circle::area() const { return 3.14159265f * _radius * _radius; }
std::string Circle::type_name() const { return "circle"; }

int add_numbers(int a, int b) { return a + b; }
float add_floats(float a, float b) { return a + b; }
