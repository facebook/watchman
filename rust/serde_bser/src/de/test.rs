use bytes::Buf;
use std::borrow::Cow;
use std::collections::HashMap;
use std::io::Cursor;

use from_reader;
use from_slice;

// For "from_reader" data in owned and for "from_slice" data is borrowed

#[derive(Debug, Deserialize, Eq, Hash, PartialEq)]
struct Bytestring<'a>(#[serde(borrow)] Cow<'a, [u8]>);

impl<'a> From<&'a [u8]> for Bytestring<'a> {
    fn from(value: &'a [u8]) -> Self {
        Bytestring(Cow::Borrowed(value))
    }
}

impl<'a, 'b> PartialEq<&'b [u8]> for Bytestring<'a> {
    fn eq(&self, rhs: &&'b [u8]) -> bool {
        self.0 == *rhs
    }
}

#[derive(Debug, Deserialize, Eq, PartialEq)]
struct BytestringArray<'a>(#[serde(borrow)] Vec<Bytestring<'a>>);

#[derive(Debug, Deserialize, Eq, PartialEq)]
struct BytestringObject<'a>(#[serde(borrow)] HashMap<Bytestring<'a>, Bytestring<'a>>);

#[derive(Debug, Deserialize, Eq, PartialEq)]
struct TwoBytestrings<'a>(
    #[serde(borrow)] Bytestring<'a>,
    #[serde(borrow)] Bytestring<'a>,
);

#[derive(Debug, Deserialize, Eq, PartialEq)]
enum BytestringVariant<'a> {
    TestUnit,
    TestNewtype(Bytestring<'a>),
    TestTuple(
        #[serde(borrow)] Bytestring<'a>,
        #[serde(borrow)] Bytestring<'a>,
    ),
    TestStruct {
        #[serde(borrow)]
        abc: Bytestring<'a>,
        #[serde(borrow)]
        def: Bytestring<'a>,
    },
}

#[derive(Debug, Deserialize, Eq, PartialEq)]
enum StringVariant {
    TestUnit,
    TestNewtype(String),
    TestTuple(String, String),
    TestStruct { abc: String, def: String },
}

#[test]
fn test_basic_array() {
    let bser_v2 =
        b"\x00\x02\x00\x00\x00\x00\x05\x11\x00\x00\x00\x00\x03\x02\x02\x03\x03Tom\x02\x03\x05Jerry";
    let decoded = from_slice::<BytestringArray>(bser_v2).unwrap();
    assert_eq!(decoded.0, vec![&b"Tom"[..], &b"Jerry"[..]]);

    let reader = Cursor::new(bser_v2.to_vec()).reader();
    let decoded: Vec<String> = from_reader(reader).unwrap();
    let expected = vec!["Tom", "Jerry"];
    assert_eq!(decoded, expected);
}

#[test]
fn test_basic_object() {
    let bser_v2 =
        b"\x00\x02\x00\x00\x00\x00\x05\x0f\x00\x00\x00\x01\x03\x01\x02\x03\x03abc\x02\x03\x03def";
    let decoded = from_slice::<BytestringObject>(bser_v2).unwrap();
    let expected = hashmap! {
        Bytestring::from(&b"abc"[..]) => Bytestring::from(&b"def"[..])
    };
    assert_eq!(decoded.0, expected);

    let reader = Cursor::new(bser_v2.to_vec()).reader();
    let decoded: HashMap<String, String> = from_reader(reader).unwrap();
    let expected = hashmap! {
        "abc".into() => "def".into()
    };
    assert_eq!(decoded, expected);
}

#[test]
fn test_basic_tuple() {
    let bser_v2 =
        b"\x00\x02\x00\x00\x00\x00\x05\x11\x00\x00\x00\x00\x03\x02\x02\x03\x03Tom\x02\x03\x05Jerry";
    let decoded = from_slice::<TwoBytestrings>(bser_v2).unwrap();
    assert_eq!(decoded.0, &b"Tom"[..]);
    assert_eq!(decoded.1, &b"Jerry"[..]);

    let reader = Cursor::new(bser_v2.to_vec()).reader();
    let decoded: (String, String) = from_reader(reader).unwrap();
    let expected: (String, String) = ("Tom".into(), "Jerry".into());
    assert_eq!(decoded, expected);
}

#[test]
fn test_bare_variant() {
    // "TestUnit"
    let bser_v2 = b"\x00\x02\x00\x00\x00\x00\x05\x0b\x00\x00\x00\x02\x03\x08TestUnit";
    let decoded = from_slice::<BytestringVariant>(bser_v2).unwrap();
    assert_eq!(decoded, BytestringVariant::TestUnit);

    let reader = Cursor::new(bser_v2.to_vec()).reader();
    let decoded: StringVariant = from_reader(reader).unwrap();
    assert_eq!(decoded, StringVariant::TestUnit);
}

#[test]
fn test_unit_variant() {
    // {"TestUnit": null}
    let bser_v2 = b"\x00\x02\x00\x00\x00\x00\x05\x0f\x00\x00\x00\x01\x03\x01\x02\x03\x08TestUnit\n";
    let decoded = from_slice::<BytestringVariant>(bser_v2).unwrap();
    assert_eq!(decoded, BytestringVariant::TestUnit);

    let reader = Cursor::new(bser_v2.to_vec()).reader();
    let decoded: StringVariant = from_reader(reader).unwrap();
    assert_eq!(decoded, StringVariant::TestUnit);
}

#[test]
fn test_newtype_variant() {
    // {"TestNewtype": "foobar"}
    let bser_v2 =
        b"\x00\x02\x00\x00\x00\x00\x05\x1a\x00\x00\x00\x01\x03\x01\x02\x03\x0bTestNewtype\
                    \x02\x03\x06foobar";
    let decoded = from_slice::<BytestringVariant>(bser_v2).unwrap();
    assert_eq!(
        decoded,
        BytestringVariant::TestNewtype((&b"foobar"[..]).into())
    );

    let reader = Cursor::new(bser_v2.to_vec()).reader();
    let decoded: StringVariant = from_reader(reader).unwrap();
    assert_eq!(decoded, StringVariant::TestNewtype("foobar".into()));
}

#[test]
fn test_tuple_variant() {
    // {"TestTuple": ["foo", "bar"]}
    let bser_v2 = b"\x00\x02\x00\x00\x00\x00\x05\x1e\x00\x00\x00\x01\x03\x01\x02\x03\tTestTuple\
                    \x00\x03\x02\x02\x03\x03foo\x02\x03\x03bar";
    let decoded = from_slice::<BytestringVariant>(bser_v2).unwrap();
    assert_eq!(
        decoded,
        BytestringVariant::TestTuple((&b"foo"[..]).into(), (&b"bar"[..]).into())
    );

    let reader = Cursor::new(bser_v2.to_vec()).reader();
    let decoded: StringVariant = from_reader(reader).unwrap();
    assert_eq!(
        decoded,
        StringVariant::TestTuple("foo".into(), "bar".into())
    );
}

#[test]
fn test_struct_variant() {
    // {"TestStruct": {"abc": "foo", "def": "bar"}}
    let bser_v2 = b"\x00\x02\x00\x00\x00\x00\x05+\x00\x00\x00\x01\x03\x01\x02\x03\nTestStruct\
                    \x01\x03\x02\x02\x03\x03abc\x02\x03\x03foo\x02\x03\x03def\x02\x03\x03bar";
    let decoded = from_slice::<BytestringVariant>(bser_v2).unwrap();
    assert_eq!(
        decoded,
        BytestringVariant::TestStruct {
            abc: (&b"foo"[..]).into(),
            def: (&b"bar"[..]).into(),
        }
    );

    let reader = Cursor::new(bser_v2.to_vec()).reader();
    let decoded: StringVariant = from_reader(reader).unwrap();
    assert_eq!(
        decoded,
        StringVariant::TestStruct {
            abc: "foo".into(),
            def: "bar".into(),
        }
    );
}

#[derive(Debug, Deserialize, Eq, PartialEq)]
struct BytestringTemplateObject<'a> {
    abc: i32,
    #[serde(borrow)]
    def: Option<Bytestring<'a>>,
    ghi: Option<i64>,
}

#[derive(Debug, Deserialize, Eq, PartialEq)]
struct TemplateObject {
    abc: i32,
    def: Option<String>,
    ghi: Option<i64>,
}

#[test]
fn test_template() {
    // Logical expansion of this template:
    // [
    //   {"abc": 123, "def": "bar", "ghi": null},
    //   {"abc": 456,               "ghi": 789},
    // ]
    //
    // The second "def" is skipped.
    let bser_v2 = b"\x00\x02\x00\x00\x00\x00\x05(\x00\x00\x00\x0b\x00\x03\x03\x02\x03\x03abc\x02\
                    \x03\x03def\x02\x03\x03ghi\x03\x02\x03{\x02\x03\x03bar\n\x04\xc8\x01\x0c\x04\
                    \x15\x03";
    let decoded = from_slice::<Vec<BytestringTemplateObject>>(bser_v2).unwrap();
    assert_eq!(
        decoded,
        vec![
            BytestringTemplateObject {
                abc: 123,
                def: Some((&b"bar"[..]).into()),
                ghi: None,
            },
            BytestringTemplateObject {
                abc: 456,
                def: None,
                ghi: Some(789),
            },
        ]
    );

    let reader = Cursor::new(bser_v2.to_vec()).reader();
    let decoded: Vec<TemplateObject> = from_reader(reader).unwrap();
    assert_eq!(
        decoded,
        vec![
            TemplateObject {
                abc: 123,
                def: Some("bar".into()),
                ghi: None,
            },
            TemplateObject {
                abc: 456,
                def: None,
                ghi: Some(789),
            },
        ]
    );
}
