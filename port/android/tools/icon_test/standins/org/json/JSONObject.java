package org.json;

import java.util.LinkedHashMap;
import java.util.Map;

/** What Mods.java uses of org.json, which on a PC is not there: a flat object of strings, numbers and string arrays. */
public class JSONObject {
    private final Map<String, Object> values = new LinkedHashMap<>();
    private String text; private int at;

    public JSONObject() {}

    public JSONObject(String source) throws JSONException {
        text = source; expect('{');
        while (peek() != '}') {
            String key = string(); expect(':'); values.put(key, value());
            if (peek() == ',') at++;
        }
    }

    private char peek() throws JSONException {
        while (at < text.length() && Character.isWhitespace(text.charAt(at))) at++;
        if (at >= text.length()) throw new JSONException("cut short");
        return text.charAt(at);
    }

    private void expect(char c) throws JSONException { if (peek() != c) throw new JSONException("wanted " + c + " at " + at); at++; }

    private String string() throws JSONException {
        expect('"'); StringBuilder s = new StringBuilder();
        for (char c; (c = text.charAt(at++)) != '"'; ) { if (c == '\\') c = text.charAt(at++); s.append(c); }
        return s.toString();
    }

    private Object value() throws JSONException {
        char c = peek();
        if (c == '"') return string();
        if (c == '[') {
            at++; JSONArray a = new JSONArray();
            while (peek() != ']') { a.items.add(value()); if (peek() == ',') at++; }
            at++; return a;
        }
        int from = at; while (at < text.length() && "-0123456789".indexOf(text.charAt(at)) >= 0) at++;
        if (from == at) throw new JSONException("no value at " + at);
        return Long.valueOf(text.substring(from, at));
    }

    public Object opt(String key) { return values.get(key); }
    public String optString(String key) { Object v = values.get(key); return v == null ? "" : String.valueOf(v); }
    public long optLong(String key) { Object v = values.get(key); return v instanceof Number ? ((Number) v).longValue() : 0; }
    public int optInt(String key) { return (int) optLong(key); }
    public JSONArray optJSONArray(String key) { Object v = values.get(key); return v instanceof JSONArray ? (JSONArray) v : null; }

    public JSONObject put(String key, Object value) throws JSONException { values.put(key, value); return this; }
    public JSONObject put(String key, long value) throws JSONException { values.put(key, value); return this; }
    public JSONObject put(String key, int value) throws JSONException { values.put(key, (long) value); return this; }

    private static void write(StringBuilder s, Object v) {
        if (v instanceof Number) { s.append(v); return; }
        if (v instanceof JSONArray) {
            s.append('['); String sep = "";
            for (Object x : ((JSONArray) v).items) { s.append(sep); write(s, x); sep = ", "; }
            s.append(']'); return;
        }
        s.append('"').append(String.valueOf(v).replace("\\", "\\\\").replace("\"", "\\\"")).append('"');
    }

    public String toString(int indent) throws JSONException {
        StringBuilder s = new StringBuilder("{\n"); String sep = "";
        for (Map.Entry<String, Object> e : values.entrySet()) {
            s.append(sep).append(" ".repeat(indent)); write(s, e.getKey()); s.append(": "); write(s, e.getValue()); sep = ",\n";
        }
        return s.append("\n}").toString();
    }
}
