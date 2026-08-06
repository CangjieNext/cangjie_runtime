package main

import (
	"bufio"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"strconv"
)

type jsonStreamWriter struct{ out *bufio.Writer }

type countingReader struct {
	reader io.Reader
	count  int64
}

func (reader *countingReader) Read(buffer []byte) (int, error) {
	n, err := reader.reader.Read(buffer)
	reader.count += int64(n)
	return n, err
}

func newJSONStreamWriter(output io.Writer) *jsonStreamWriter {
	return &jsonStreamWriter{out: bufio.NewWriter(output)}
}

func (w *jsonStreamWriter) raw(value string) error {
	_, err := w.out.WriteString(value)
	return err
}

func (w *jsonStreamWriter) string(value string) error {
	encoded, err := json.Marshal(value)
	if err != nil {
		return err
	}
	_, err = w.out.Write(encoded)
	return err
}

func (w *jsonStreamWriter) name(value string) error {
	if err := w.string(value); err != nil {
		return err
	}
	return w.raw(":")
}

func (w *jsonStreamWriter) int(value int64) error {
	return w.raw(strconv.FormatInt(value, 10))
}

func (w *jsonStreamWriter) float(value float64) error {
	encoded, err := json.Marshal(value)
	if err != nil {
		return err
	}
	_, err = w.out.Write(encoded)
	return err
}

func expectDelimiter(decoder *json.Decoder, expected json.Delim) error {
	token, err := decoder.Token()
	if err != nil {
		return err
	}
	if delimiter, ok := token.(json.Delim); !ok || delimiter != expected {
		return fmt.Errorf("expected JSON delimiter %q, got %v", expected, token)
	}
	return nil
}

func readName(decoder *json.Decoder) (string, error) {
	token, err := decoder.Token()
	if err != nil {
		return "", err
	}
	name, ok := token.(string)
	if !ok {
		return "", fmt.Errorf("expected JSON object field name, got %v", token)
	}
	return name, nil
}

func writeField(writer *jsonStreamWriter, first *bool, name string) error {
	if !*first {
		if err := writer.raw(","); err != nil {
			return err
		}
	}
	*first = false
	return writer.name(name)
}

func copyJSONValue(decoder *json.Decoder, writer *jsonStreamWriter) error {
	var raw json.RawMessage
	if err := decoder.Decode(&raw); err != nil {
		return err
	}
	_, err := writer.out.Write(raw)
	return err
}

func copyJSONToken(decoder *json.Decoder, writer *jsonStreamWriter, token json.Token) error {
	switch value := token.(type) {
	case json.Delim:
		switch value {
		case '{':
			if err := writer.raw("{"); err != nil {
				return err
			}
			first := true
			for decoder.More() {
				name, err := readName(decoder)
				if err != nil {
					return err
				}
				if err := writeField(writer, &first, name); err != nil {
					return err
				}
				if err := copyJSONValue(decoder, writer); err != nil {
					return err
				}
			}
			if err := expectDelimiter(decoder, '}'); err != nil {
				return err
			}
			return writer.raw("}")
		case '[':
			if err := writer.raw("["); err != nil {
				return err
			}
			first := true
			for decoder.More() {
				if !first {
					if err := writer.raw(","); err != nil {
						return err
					}
				}
				first = false
				if err := copyJSONValue(decoder, writer); err != nil {
					return err
				}
			}
			if err := expectDelimiter(decoder, ']'); err != nil {
				return err
			}
			return writer.raw("]")
		}
		return fmt.Errorf("unexpected JSON delimiter %q", value)
	case json.Number:
		return writer.raw(value.String())
	case string:
		return writer.string(value)
	case bool:
		return writer.raw(strconv.FormatBool(value))
	case nil:
		return writer.raw("null")
	default:
		return fmt.Errorf("unsupported JSON token %T", token)
	}
}

func skipJSONValue(decoder *json.Decoder) error {
	token, err := decoder.Token()
	if err != nil {
		return err
	}
	if delimiter, ok := token.(json.Delim); ok {
		switch delimiter {
		case '{':
			for decoder.More() {
				if _, err := readName(decoder); err != nil {
					return err
				}
				if err := skipJSONValue(decoder); err != nil {
					return err
				}
			}
			return expectDelimiter(decoder, '}')
		case '[':
			for decoder.More() {
				if err := skipJSONValue(decoder); err != nil {
					return err
				}
			}
			return expectDelimiter(decoder, ']')
		}
	}
	return nil
}

func transformIncrement(decoder *json.Decoder, writer *jsonStreamWriter) error {
	token, err := decoder.Token()
	if err != nil {
		return err
	}
	if number, ok := token.(json.Number); ok {
		if value, err := number.Int64(); err == nil {
			return writer.int(value + 1)
		}
		if value, err := number.Float64(); err == nil {
			return writer.int(int64(value) + 1)
		}
	}
	return copyJSONToken(decoder, writer, token)
}

func transformDouble(decoder *json.Decoder, writer *jsonStreamWriter) error {
	token, err := decoder.Token()
	if err != nil {
		return err
	}
	if number, ok := token.(json.Number); ok {
		if value, err := number.Float64(); err == nil {
			return writer.float(value * 2.0)
		}
	}
	return copyJSONToken(decoder, writer, token)
}

type objectKind int

const (
	countersObject objectKind = iota
	eventObject
	userObject
	attrsObject
	customObject
)

func transformObject(decoder *json.Decoder, writer *jsonStreamWriter, kind objectKind, eventIndex int) error {
	token, err := decoder.Token()
	if err != nil {
		return err
	}
	if delimiter, ok := token.(json.Delim); !ok || delimiter != '{' {
		return copyJSONToken(decoder, writer, token)
	}
	if err := writer.raw("{"); err != nil {
		return err
	}
	first := true
	for decoder.More() {
		name, err := readName(decoder)
		if err != nil {
			return err
		}
		if err := writeField(writer, &first, name); err != nil {
			return err
		}
		switch kind {
		case countersObject:
			if name == "in" || name == "ok" {
				err = transformIncrement(decoder, writer)
			} else {
				err = copyJSONValue(decoder, writer)
			}
		case eventObject:
			switch name {
			case "seq":
				err = transformIncrement(decoder, writer)
			case "score":
				err = transformDouble(decoder, writer)
			case "user":
				err = transformObject(decoder, writer, userObject, eventIndex)
			case "custom":
				err = transformObject(decoder, writer, customObject, eventIndex)
			default:
				err = copyJSONValue(decoder, writer)
			}
		case userObject:
			if name == "attrs" {
				err = transformObject(decoder, writer, attrsObject, eventIndex)
			} else {
				err = copyJSONValue(decoder, writer)
			}
		case attrsObject:
			if name == "credits" {
				err = transformDouble(decoder, writer)
			} else {
				err = copyJSONValue(decoder, writer)
			}
		case customObject:
			if name == "product_ids" {
				err = transformProductIDs(decoder, writer, eventIndex)
			} else {
				err = copyJSONValue(decoder, writer)
			}
		}
		if err != nil {
			return err
		}
	}
	if err := expectDelimiter(decoder, '}'); err != nil {
		return err
	}
	if kind == eventObject {
		if err := writeField(writer, &first, "server_marker"); err != nil {
			return err
		}
		if err := writer.string(fmt.Sprintf("m-%d", eventIndex)); err != nil {
			return err
		}
	}
	return writer.raw("}")
}

func transformEvents(decoder *json.Decoder, writer *jsonStreamWriter) error {
	token, err := decoder.Token()
	if err != nil {
		return err
	}
	if delimiter, ok := token.(json.Delim); !ok || delimiter != '[' {
		return copyJSONToken(decoder, writer, token)
	}
	if err := writer.raw("["); err != nil {
		return err
	}
	first := true
	for index := 0; decoder.More(); index++ {
		if !first {
			if err := writer.raw(","); err != nil {
				return err
			}
		}
		first = false
		if err := transformObject(decoder, writer, eventObject, index); err != nil {
			return err
		}
	}
	if err := expectDelimiter(decoder, ']'); err != nil {
		return err
	}
	return writer.raw("]")
}

func transformProductIDs(decoder *json.Decoder, writer *jsonStreamWriter, eventIndex int) error {
	token, err := decoder.Token()
	if err != nil {
		return err
	}
	if delimiter, ok := token.(json.Delim); !ok || delimiter != '[' {
		return copyJSONToken(decoder, writer, token)
	}
	if err := writer.raw("["); err != nil {
		return err
	}
	first := true
	for decoder.More() {
		if !first {
			if err := writer.raw(","); err != nil {
				return err
			}
		}
		first = false
		if err := copyJSONValue(decoder, writer); err != nil {
			return err
		}
	}
	if err := expectDelimiter(decoder, ']'); err != nil {
		return err
	}
	if !first {
		if err := writer.raw(","); err != nil {
			return err
		}
	}
	if err := writer.int(int64(eventIndex) + 9999); err != nil {
		return err
	}
	return writer.raw("]")
}

func transformRequestJSONStream(input io.Reader, output io.Writer, sequence int64) error {
	decoder := json.NewDecoder(input)
	decoder.UseNumber()
	if err := expectDelimiter(decoder, '{'); err != nil {
		return fmt.Errorf("root json must be object: %w", err)
	}
	writer := newJSONStreamWriter(output)
	if err := writer.raw("{"); err != nil {
		return err
	}
	first := true
	for decoder.More() {
		name, err := readName(decoder)
		if err != nil {
			return err
		}
		if err := writeField(writer, &first, name); err != nil {
			return err
		}
		if name == "counters" {
			err = transformObject(decoder, writer, countersObject, 0)
		} else if name == "events" {
			err = transformEvents(decoder, writer)
		} else {
			err = copyJSONValue(decoder, writer)
		}
		if err != nil {
			return err
		}
	}
	if err := expectDelimiter(decoder, '}'); err != nil {
		return err
	}
	if err := writeField(writer, &first, "processed_seq"); err != nil {
		return err
	}
	if err := writer.int(sequence); err != nil {
		return err
	}
	if err := writeField(writer, &first, "server_ok"); err != nil {
		return err
	}
	if err := writer.raw("true"); err != nil {
		return err
	}
	if err := writer.raw("}"); err != nil {
		return err
	}
	return writer.out.Flush()
}

func validateServerOKStream(input io.Reader) error {
	decoder := json.NewDecoder(input)
	decoder.UseNumber()
	if err := expectDelimiter(decoder, '{'); err != nil {
		return fmt.Errorf("response root json must be object: %w", err)
	}
	serverOK := false
	found := false
	for decoder.More() {
		name, err := readName(decoder)
		if err != nil {
			return err
		}
		if name != "server_ok" {
			if err := skipJSONValue(decoder); err != nil {
				return err
			}
			continue
		}
		token, err := decoder.Token()
		if err != nil {
			return err
		}
		value, ok := token.(bool)
		if !ok {
			return errors.New("response server_ok must be boolean")
		}
		found = true
		serverOK = value
	}
	if err := expectDelimiter(decoder, '}'); err != nil {
		return err
	}
	if !found {
		return errors.New("response missing server_ok")
	}
	if !serverOK {
		return errors.New("response server_ok=false")
	}
	return nil
}
