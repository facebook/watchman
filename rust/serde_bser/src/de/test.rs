use std::borrow::Cow;
use std::collections::HashMap;

use from_slice;

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

#[test]
fn test_basic_array() {
    let bser_v2 =
        b"\x00\x02\x00\x00\x00\x00\x05\x11\x00\x00\x00\x00\x03\x02\x02\x03\x03Tom\x02\x03\x05Jerry";
    let decoded = from_slice::<BytestringArray>(bser_v2).unwrap();
    assert_eq!(decoded.0, vec![&b"Tom"[..], &b"Jerry"[..]]);
}

#[test]
fn test_basic_object() {
    let bser_v2 =
        b"\x00\x02\x00\x00\x00\x00\x05\x0f\x00\x00\x00\x01\x03\x01\x02\x03\x03abc\x02\x03\x03def";
    let decoded = from_slice::<BytestringObject>(bser_v2).unwrap();
    let mut expected = HashMap::new();
    expected.insert(Bytestring::from(&b"abc"[..]), Bytestring::from(&b"def"[..]));
    assert_eq!(decoded.0, expected);
}

#[test]
fn test_basic_tuple() {
    let bser_v2 =
        b"\x00\x02\x00\x00\x00\x00\x05\x11\x00\x00\x00\x00\x03\x02\x02\x03\x03Tom\x02\x03\x05Jerry";
    let decoded = from_slice::<TwoBytestrings>(bser_v2).unwrap();
    assert_eq!(decoded.0, &b"Tom"[..]);
    assert_eq!(decoded.1, &b"Jerry"[..]);
}

#[test]
fn test_bare_variant() {
    // "TestUnit"
    let bser_v2 = b"\x00\x02\x00\x00\x00\x00\x05\x0b\x00\x00\x00\x02\x03\x08TestUnit";
    let decoded = from_slice::<BytestringVariant>(bser_v2).unwrap();
    assert_eq!(decoded, BytestringVariant::TestUnit);
}

#[test]
fn test_unit_variant() {
    // {"TestUnit": null}
    let bser_v2 = b"\x00\x02\x00\x00\x00\x00\x05\x0f\x00\x00\x00\x01\x03\x01\x02\x03\x08TestUnit\n";
    let decoded = from_slice::<BytestringVariant>(bser_v2).unwrap();
    assert_eq!(decoded, BytestringVariant::TestUnit);
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
}

#[derive(Debug, Deserialize, Eq, PartialEq)]
struct TemplateObject<'a> {
    abc: i32,
    #[serde(borrow)]
    def: Option<Bytestring<'a>>,
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
    let decoded = from_slice::<Vec<TemplateObject>>(bser_v2).unwrap();
    assert_eq!(
        decoded,
        vec![
            TemplateObject {
                abc: 123,
                def: Some((&b"bar"[..]).into()),
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
