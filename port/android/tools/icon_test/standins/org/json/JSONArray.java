package org.json;

import java.util.ArrayList;
import java.util.Collection;
import java.util.List;

/** What Mods.java uses of org.json, which on a PC is not there: an array of strings. */
public class JSONArray {
    final List<Object> items = new ArrayList<>();
    JSONArray() {}
    public JSONArray(Collection<?> from) { items.addAll(from); }
    public int length() { return items.size(); }
    public String optString(int i) { return String.valueOf(items.get(i)); }
}
