// server/student.h
#pragma once

#include "../common/json.h"
#include <string>

struct Student {
    int id = 0;
    std::string name;
    int age = 0;
    std::string grade;

    json::Value toJson() const {
        json::Object o;
        o["id"]    = json::Value(id);
        o["name"]  = json::Value(name);
        o["age"]   = json::Value(age);
        o["grade"] = json::Value(grade);
        return json::Value(std::move(o));
    }

    static Student fromJson(const json::Value& v) {
        Student s;
        s.id    = v.getInt("id", 0);
        s.name  = v.getString("name", "");
        s.age   = v.getInt("age", 0);
        s.grade = v.getString("grade", "");
        return s;
    }
};
